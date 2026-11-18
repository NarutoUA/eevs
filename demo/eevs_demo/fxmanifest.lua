fx_version 'cerulean'
game 'gta5'
lua54 'yes'

name 'eevs_demo'
author 'EEVS'
description 'Basic interactive command & NUI visualizer showcase for EEVS (Enhanced External Voice Server)'
version '1.0.0'

ui_page 'html/index.html'

files {
    'html/index.html',
    'html/style.css',
    'html/js/core.js',
    'html/js/tabs.js',
    'html/js/npc_tab.js'
}

client_scripts {
    'shared/npc_catalog.lua',
    'client/state.lua',
    'client/npc.lua',
    'client/ui_hud.lua'
}

server_scripts {
    '@eevs/lib/eevs.lua',
    'shared/npc_catalog.lua',
    'server/llm_client.lua',
    'server/npc_interaction.lua',
    'server/commands_and_rpc.lua'
}

dependencies {
    'eevs'
}
