-- ============================================================================
-- EEVS Demo - Server OpenAI-Compatible LLM Client & Tool Parser
-- ============================================================================

DemoServer = {
    lastRecordedClipByPlayer = {},
    activeNpcInteractionByPlayer = {},
    defaultProximityChannel = nil,
    cachedLlmModelId = nil
}

function DemoServer.EevsRequest(method, path, payload, cb)
    return EEVS.Request(method, path, payload, cb)
end

-- Keep backwards-compatible export on `eevs_demo` delegating to `eevs`
exports('Request', DemoServer.EevsRequest)

function DemoServer.NotifyPlayer(src, msg, isError)
    local text = tostring(msg)
    local lower = string.lower(text)
    if isError or lower:find('fail', 1, true) or lower:find('error', 1, true) then
        print(('[EEVS] [ERROR] (player=%s) %s'):format(tostring(src), text))
    elseif src == 0 then
        print('[EEVS] ' .. text)
    end
    if src ~= 0 then
        TriggerClientEvent('eevs_demo:notify', src, text)
    end
end

local function resolveLlmApiBaseUrl()
    local raw = GetConvar('llm_api_url', 'http://127.0.0.1:1338/v1')
    raw = raw:gsub('^%s*(.-)%s*$', '%1'):gsub('/+$', '')
    if not raw:match('^https?://') then
        raw = 'http://' .. raw
    end
    raw = raw:gsub('/chat/completions$', '')
    if not raw:match('/v1$') then
        raw = raw .. '/v1'
    end
    return raw
end

local function resolveLlmHeaders()
    local headers = {
        ['Content-Type'] = 'application/json'
    }
    local apiKey = GetConvar('llm_api_key', ''):gsub('^%s*(.-)%s*$', '%1')
    if apiKey ~= '' and apiKey ~= 'none' then
        headers['Authorization'] = 'Bearer ' .. apiKey
    end
    return headers
end

function DemoServer.FetchLlmModelsList(cb)
    local baseUrl = resolveLlmApiBaseUrl()
    PerformHttpRequest(baseUrl .. '/models', function(status, body)
        local models = {}
        if status == 200 and body and #body > 0 then
            local ok, decoded = pcall(json.decode, body)
            if ok and type(decoded) == 'table' and type(decoded.data) == 'table' then
                for _, item in ipairs(decoded.data) do
                    if type(item) == 'table' and type(item.id) == 'string' and item.id ~= '' then
                        table.insert(models, item.id)
                    end
                end
            end
        end
        if models[1] and not DemoServer.cachedLlmModelId then
            DemoServer.cachedLlmModelId = models[1]
        end
        cb(status == 200, models, baseUrl)
    end, 'GET', '', resolveLlmHeaders())
end

local function ensureLlmModelId(baseUrl, requestedModel, cb)
    if type(requestedModel) == 'string' and requestedModel ~= '' then
        cb(requestedModel)
        return
    end
    local explicitModel = GetConvar('llm_model', ''):gsub('^%s*(.-)%s*$', '%1')
    if explicitModel ~= '' then
        cb(explicitModel)
        return
    end
    if DemoServer.cachedLlmModelId and DemoServer.cachedLlmModelId ~= '' then
        cb(DemoServer.cachedLlmModelId)
        return
    end
    PerformHttpRequest(baseUrl .. '/models', function(status, body)
        if status == 200 and body and #body > 0 then
            local ok, decoded = pcall(json.decode, body)
            if ok and type(decoded) == 'table' and type(decoded.data) == 'table' and decoded.data[1] and decoded.data[1].id then
                DemoServer.cachedLlmModelId = decoded.data[1].id
                cb(DemoServer.cachedLlmModelId)
                return
            end
        end
        cb(nil)
    end, 'GET', '', resolveLlmHeaders())
end

local TOOL_CALLING_INSTRUCTIONS = [[

Always respond by calling the `npc_respond` tool with your spoken `speech` (or `""` for a silent physical reaction) and your chosen `action` (or `"none"` if only speaking).]]

local function extractJsonObjectFromText(rawText)
    if type(rawText) ~= 'string' or rawText == '' then
        return nil
    end
    local lastBlock = nil
    for block in rawText:gmatch('(%b{})') do
        lastBlock = block
    end
    if not lastBlock then
        return nil
    end
    local ok, parsed = pcall(json.decode, lastBlock)
    if ok and type(parsed) == 'table' then
        return parsed
    end
    return nil
end

local function normalizeActionAndArgs(parsedTable, fallbackActionName)
    if type(parsedTable) ~= 'table' then
        return '', 'none', {}
    end

    local outerName = parsedTable.tool or parsedTable.name or fallbackActionName
    if type(parsedTable['function']) == 'table' then
        outerName = parsedTable['function'].name or outerName
        local fnArgs = parsedTable['function'].arguments
        if type(fnArgs) == 'string' then
            local ok, dec = pcall(json.decode, fnArgs)
            if ok and type(dec) == 'table' then
                parsedTable = dec
            end
        elseif type(fnArgs) == 'table' then
            parsedTable = fnArgs
        end
    elseif type(parsedTable.arguments) == 'table' then
        parsedTable = parsedTable.arguments
    elseif type(parsedTable.arguments) == 'string' then
        local ok, dec = pcall(json.decode, parsedTable.arguments)
        if ok and type(dec) == 'table' then
            parsedTable = dec
        end
    elseif type(parsedTable.parameters) == 'table' then
        parsedTable = parsedTable.parameters
    end

    local rawAction = tostring(parsedTable.action or parsedTable.tool or parsedTable.name or outerName or 'none'):gsub('^%s*(.-)%s*$', '%1')
    local actionName = string.lower(rawAction)
    if actionName == 'npc_respond' then
        actionName = 'none'
    end
    if actionName ~= 'none' and (not NPC_ACTIONS_BY_NAME or not NPC_ACTIONS_BY_NAME[actionName]) then
        actionName = 'none'
    end

    local speech = tostring(parsedTable.speech or parsedTable.reply or parsedTable.text or ''):gsub('^%s*(.-)%s*$', '%1')
    local actionArgs = {}
    for k, v in pairs(parsedTable) do
        if k ~= 'speech' and k ~= 'reply' and k ~= 'text' and k ~= 'action' and k ~= 'tool' and k ~= 'name' and k ~= 'arguments' and k ~= 'parameters' and k ~= 'function' then
            actionArgs[k] = v
        end
    end
    return speech, actionName, actionArgs
end

local function parseLlmMessageWithTools(msg, fallbackText)
    if type(msg) == 'table' and type(msg.tool_calls) == 'table' and msg.tool_calls[1] then
        local tc = msg.tool_calls[1]
        local fn = tc and tc['function']
        local fnName = fn and tostring(fn.name or ''):gsub('^%s*(.-)%s*$', '%1') or ''

        local parsedArgs = {}
        if fn and type(fn.arguments) == 'string' and fn.arguments ~= '' then
            local ok, decoded = pcall(json.decode, fn.arguments)
            if ok and type(decoded) == 'table' then
                parsedArgs = decoded
            end
        elseif fn and type(fn.arguments) == 'table' then
            parsedArgs = fn.arguments
        end

        local speechFromArgs, actionName, actionArgs = normalizeActionAndArgs(parsedArgs, fnName)
        local msgContent = type(msg.content) == 'string' and msg.content:gsub('^%s*(.-)%s*$', '%1') or ''
        local finalSpeech = (speechFromArgs ~= '') and speechFromArgs or msgContent
        return finalSpeech, actionName, actionArgs
    end

    local rawContent = (type(msg) == 'table' and type(msg.content) == 'string' and msg.content) or fallbackText or ''
    local trimmed = rawContent:gsub('^%s*(.-)%s*$', '%1')
    local fromContent = extractJsonObjectFromText(trimmed)
    if fromContent then
        local speech, action, args = normalizeActionAndArgs(fromContent, nil)
        if speech ~= '' or action ~= 'none' then
            return speech, action, args
        end
    end
    if trimmed ~= '' then
        return trimmed, 'none', {}
    end

    local reasoningText = type(msg) == 'table' and (msg.reasoning or msg.reasoning_content) or nil
    local fromReasoning = extractJsonObjectFromText(reasoningText)
    if fromReasoning then
        local speech, action, args = normalizeActionAndArgs(fromReasoning, nil)
        if speech ~= '' or action ~= 'none' then
            return speech, action, args
        end
    end

    return '', 'none', {}
end

function DemoServer.QueryLlmApi(userPrompt, systemPrompt, requestedModel, enabledActions, cb)
    if type(requestedModel) == 'function' then
        cb = requestedModel
        requestedModel = nil
        enabledActions = nil
    elseif type(enabledActions) == 'function' then
        cb = enabledActions
        enabledActions = nil
    end
    local baseUrl = resolveLlmApiBaseUrl()
    local baseSys = (systemPrompt and systemPrompt ~= '')
        and systemPrompt
        or 'You are an interactive NPC in Los Santos (GTA V). Keep your spoken reply natural and concise (1 to 2 sentences).'
    local tools = (type(BuildOpenAiNpcTools) == 'function') and BuildOpenAiNpcTools(enabledActions) or {}

    ensureLlmModelId(baseUrl, requestedModel, function(modelId)
        local payload = {
            model = modelId,
            messages = {
                { role = 'system', content = baseSys .. TOOL_CALLING_INSTRUCTIONS },
                { role = 'user', content = userPrompt }
            },
            tools = (#tools > 0) and tools or nil,
            tool_choice = (#tools > 0) and { type = 'function', ['function'] = { name = 'npc_respond' } } or nil,
            temperature = 0.7,
            max_tokens = 512
        }

        PerformHttpRequest(baseUrl .. '/chat/completions', function(status, respBody)
            if status == 200 and respBody and #respBody > 0 then
                local ok, decoded = pcall(json.decode, respBody)
                if ok and type(decoded) == 'table' and type(decoded.choices) == 'table' and decoded.choices[1] then
                    local msg = decoded.choices[1].message
                    local replyText, actionName, actionArgs = parseLlmMessageWithTools(msg, decoded.choices[1].text)
                    if replyText ~= '' or actionName ~= 'none' then
                        cb(true, replyText, actionName, actionArgs)
                        return
                    end
                end
            else
                DemoServer.cachedLlmModelId = nil
            end
            cb(false, ('LLM API error at %s (HTTP %s: %s)'):format(baseUrl, tostring(status or 0), tostring(respBody or 'unreachable')), 'none', {})
        end, 'POST', json.encode(payload), resolveLlmHeaders())
    end)
end
