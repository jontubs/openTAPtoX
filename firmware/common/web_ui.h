#ifndef OPENTAPTOX_WEB_UI_H
#define OPENTAPTOX_WEB_UI_H

#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>openTAPtoX</title>
  <style>
    body{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;margin:0 auto;max-width:1100px;padding:20px;line-height:1.4;background:#f6f8fa;color:#17212b}
    h1,h2{margin:0 0 12px 0}
    h1{font-size:28px}
    h2{font-size:18px}
    section{margin:0 0 26px 0;padding-top:18px;border-top:1px solid #d8dee4}
    header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:8px}
    header h1,.section-heading h2{margin:0}
    table{border-collapse:collapse;width:100%}
    th,td{border:1px solid #bbb;padding:6px;vertical-align:top;font-size:13px}
    input,select{box-sizing:border-box;width:100%;padding:7px;border:1px solid #9da8b3;border-radius:4px;background:#fff}
    button{padding:7px 10px;margin:2px;border:1px solid #65717d;border-radius:4px;background:#fff;color:#17212b;cursor:pointer}
    button:hover{background:#edf3f7}
    button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #89c8e8;outline-offset:2px}
    .icon-button{display:grid;place-items:center;width:32px;height:32px;padding:0;border-radius:50%;font-weight:700;font-size:16px;line-height:1}
    .section-heading{display:flex;align-items:center;gap:8px;margin-bottom:12px}
    .start-flow{border-top:0;padding-top:8px}
    .setup-list{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:0;padding:0;list-style:none}
    .setup-step{min-height:68px;padding:10px;border-left:4px solid #aab4bd;background:#fff;font-size:13px}
    .setup-step strong{display:block;font-size:14px}
    .setup-step.done{border-left-color:#208653}
    .setup-step.wait{border-left-color:#c78b16}
    .setup-state{display:block;margin-top:4px;color:#59636d;font-size:12px}
    .switch-row{display:flex;align-items:center;gap:9px;margin:8px 0 12px}
    .switch-row input{width:18px;height:18px}
    details{margin-top:12px;border-top:1px solid #d8dee4;padding-top:10px}
    summary{cursor:pointer;font-weight:600}
    dialog{max-width:620px;border:1px solid #65717d;border-radius:6px;padding:20px;box-shadow:0 12px 40px rgba(0,0,0,.25)}
    dialog::backdrop{background:rgba(23,33,43,.42)}
    @media(max-width:720px){body{padding:14px}.setup-list{grid-template-columns:1fr 1fr}th,td{font-size:12px;padding:5px}}
    pre{white-space:pre-wrap;border:1px solid #bbb;padding:10px;min-height:80px}
    label{display:block;font-size:12px;margin-bottom:4px}
    small{color:#555}
    .frame-line{padding:10px;border:1px solid #bbb;font-family:monospace;font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    .frame-ok{background:#e8f7e8;border-color:#67b567;color:#0f5c0f}
    .frame-bad{background:#fdeaea;border-color:#d66;color:#8a1f1f}
    .tap-link{padding:14px 16px;margin:12px 0;border:2px solid #bbb;font-weight:700;font-size:18px}
    .tap-link-ok{background:#dff3df;border-color:#2e8b57;color:#155724}
    .tap-link-bad{background:#f8d7da;border-color:#b02a37;color:#842029}
    .tap-version{padding:8px 10px;margin:-4px 0 12px 0;border:1px solid #bbb;background:#f7f7f7;font-family:monospace;font-size:13px;white-space:pre-wrap}
    .command-notice{display:none;position:fixed;right:16px;bottom:16px;z-index:20;max-width:min(420px,calc(100vw - 32px));padding:10px 12px;border:1px solid #4a7d9b;background:#e8f5fb;color:#17394b;font-size:14px;box-shadow:0 6px 18px rgba(23,33,43,.2)}
    .command-notice.show{display:block}
    .command-notice.error{border-color:#b02a37;background:#f8d7da;color:#842029}
  </style>
</head>
<body>
  <header><h1>openTAPtoX</h1></header>
  <p><small>Unofficial community project, not affiliated with or endorsed by Tigo.</small></p>
  <div id="command_notice" class="command-notice" role="status" aria-live="polite"></div>
  <section class="start-flow">
    <div class="section-heading"><h2>Start</h2><button class="icon-button" type="button" title="Show setup guide" aria-label="Show setup guide" onclick="openHowTo()">?</button></div>
    <ol class="setup-list">
      <li id="setup_tap" class="setup-step wait"><strong>1. TAP connection</strong><span class="setup-state">Checking...</span></li>
      <li id="setup_polling" class="setup-step wait"><strong>2. Polling</strong><span class="setup-state">Checking...</span></li>
      <li id="setup_nodes" class="setup-step wait"><strong>3. Optimizers</strong><span class="setup-state">Checking...</span></li>
      <li id="setup_power" class="setup-step wait"><strong>4. Live data</strong><span class="setup-state">Waiting for reports</span></li>
    </ol>
  </section>
  <section>
    <p>
      <strong>Gateway:</strong> <span id="gateway_id_value">-</span>
      <strong>Next packet:</strong> <span id="next_packet_value">-</span>
      <strong>IP:</strong> <span id="ip_value">-</span>
      <strong>Firmware:</strong> <span id="firmware_version_value">-</span>
    </p>
    <div id="tap_link_box" class="tap-link tap-link-bad">TAP connection: unknown</div>
    <div id="tap_version_box" class="tap-version">TAP version: -</div>
    <div id="last_frame_line" class="frame-line" title="No TAP frame received yet.">No TAP frame received yet.</div>
  </section>
  <section>
    <h2>Device</h2>
    <p><button onclick="rebootDevice()">Reboot ESP</button></p>
  </section>
  <section>
    <h2>MQTT Settings</h2>
    <table>
      <tbody>
        <tr><td style="width:220px"><label for="mqtt_host">Host</label></td><td><input id="mqtt_host" placeholder="mqtt.example.local"></td></tr>
        <tr><td><label for="mqtt_port">Port</label></td><td><input id="mqtt_port" type="number" min="1" max="65535" step="1" placeholder="1883"></td></tr>
        <tr><td><label for="mqtt_base_topic">Base Topic</label></td><td><input id="mqtt_base_topic" placeholder="openTAPtoX"></td></tr>
        <tr><td><label for="mqtt_username">Username</label></td><td><input id="mqtt_username" autocomplete="username"></td></tr>
        <tr><td><label for="mqtt_password">Password</label></td><td><input id="mqtt_password" type="password" autocomplete="current-password" placeholder="Leave empty to keep current password"></td></tr>
      </tbody>
    </table>
    <p><small id="mqtt_settings_status">MQTT settings not loaded.</small></p>
    <p><button onclick="saveMqttSettings()">Save MQTT Settings</button> <button onclick="loadMqttSettings()">Reload MQTT Settings</button></p>
  </section>
  <section>
    <h2>Live Data</h2>
    <table>
      <tbody>
        <tr><td style="width:220px">Uptime</td><td id="uptime_value">-</td></tr>
        <tr><td>WiFi Mode</td><td id="wifi_mode_value">-</td></tr>
        <tr><td>Hostname</td><td id="hostname_value">-</td></tr>
        <tr><td>MQTT Connected</td><td id="mqtt_value">-</td></tr>
        <tr><td>Free Heap</td><td id="free_heap_value">-</td></tr>
        <tr><td>Gateway Long Address</td><td id="gateway_long_addr_value">-</td></tr>
        <tr><td>Known Nodes</td><td id="node_count_value">-</td></tr>
        <tr><td>Power Reports</td><td id="power_count_value">-</td></tr>
        <tr><td>Message Counter</td><td id="message_counter_value">-</td></tr>
        <tr><td>CRC Errors</td><td id="frames_crc_error_value">-</td></tr>
        <tr><td>Polls Sent</td><td id="polls_sent_value">-</td></tr>
        <tr><td>Poll Timeouts</td><td id="poll_timeouts_value">-</td></tr>
        <tr><td>Polling</td><td id="polling_value">-</td></tr>
        <tr><td>Panel Field Count</td><td id="panel_field_count_value">-</td></tr>
        <tr><td>TAP Version</td><td id="version_text_value">-</td></tr>
        <tr><td>Live Sum Power</td><td id="live_sum_input_w_value">-</td></tr>
        <tr><td>Held Sum Power</td><td id="held_sum_input_w_value">-</td></tr>
        <tr><td>Fresh Nodes</td><td id="fresh_nodes_value">-</td></tr>
        <tr><td>Stale Nodes</td><td id="stale_nodes_value">-</td></tr>
        <tr><td>Expired Nodes</td><td id="expired_nodes_value">-</td></tr>
        <tr><td>Newest Sample Age</td><td id="newest_sample_age_ms_value">-</td></tr>
        <tr><td>Oldest Sample Age</td><td id="oldest_sample_age_ms_value">-</td></tr>
        <tr><td>Average Sample Age</td><td id="avg_sample_age_ms_value">-</td></tr>
        <tr><td>Last RSD Control</td><td id="rsd_control_state_value">-</td></tr>
        <tr><td>Active Command</td><td id="command_name_value">-</td></tr>
        <tr><td>Command State</td><td id="command_state_value">-</td></tr>
      </tbody>
    </table>
  </section>
  <section>
    <h2>Optimizer Configuration</h2>
    <p><small>Assign detected optimizers to A1..An, then save the mapping. Manual long-address entry remains available as a fallback.</small></p>
    <label class="switch-row" for="polling_toggle"><input id="polling_toggle" type="checkbox" onchange="togglePolling()"><span>Enable TAP polling</span></label>
    <p style="margin-top:12px">
      <label for="panel_count">How many optimizers?</label>
      <input id="panel_count" type="number" min="1" step="1">
    </p>
    <table>
      <thead>
        <tr><th style="width:90px">Slot</th><th style="width:120px">Label</th><th>Detected Optimizer / Long Address</th></tr>
      </thead>
      <tbody id="config_rows"></tbody>
    </table>
    <p><button onclick="requestNodeTable()">Refresh node table</button> <button onclick="refreshHeavyData()">Refresh details</button> <button onclick="saveConfig()">Save panel mapping</button></p>
    <details>
      <summary>Advanced recovery</summary>
      <p><small>Only use this when instructed during diagnostics. A TAP acknowledgement does not prove RF delivery to an optimizer.</small></p>
      <button onclick="releaseOptimizers()">Release optimizers (RSD run)</button>
      <button onclick="rewritePvConfig()">Rewrite PV reporting configuration</button>
    </details>
  </section>
  <section><details><summary>Detected node diagnostics</summary><pre id="nodes"></pre></details></section>
  <section>
    <h2>Live Power</h2>
    <table>
      <thead>
        <tr><th>Panel</th><th>Node</th><th>Short</th><th>LongAddr</th><th>Power</th><th>Temp</th><th>RSSI</th><th>Age ms</th><th>Fresh</th></tr>
      </thead>
      <tbody id="power_rows"></tbody>
    </table>
  </section>
  <section><details><summary>Events</summary><pre id="events"></pre></details></section>
  <dialog id="howto_dialog">
    <div class="section-heading"><h2>How to start</h2><button class="icon-button" type="button" title="Close" aria-label="Close" onclick="closeHowTo()">x</button></div>
    <ol>
      <li>Power the TAP and ESP, then wait for a green TAP connection status.</li>
      <li>Set MQTT only when required and save the settings.</li>
      <li>Enable TAP polling and refresh the node table. Wait until the expected optimizers appear.</li>
      <li>If the inverter remains RSD-locked in daylight, use Release optimizers once under Advanced recovery.</li>
      <li>Assign each detected optimizer to A1..An and save the panel mapping.</li>
      <li>Leave the controller running. Live power appears when the TAP receives optimizer reports.</li>
    </ol>
    <p><small>Advanced recovery commands are not part of normal setup.</small></p>
  </dialog>
  <script src="/app.js"></script>
</body>
</html>
)HTML";

static const char APP_JS[] PROGMEM = R"JS(
let panelMapCache=[];
let discoveredNodesCache=[];
let uptimeBaseMs=0;
let uptimeSyncMs=0;
let lastFrameCounterSeen=-1;
let lastFrameAgeBaseMs=0;
let lastFrameAgeSyncMs=0;
let maxOptimizers=1;
  let configDirty=false;
  let refreshBusy=false;
  let fetchQueue=Promise.resolve();
  let commandNoticeTimer=0;
const ramTotalBytes=327680;
function formatRamUsage(freeHeap){const used=Math.max(0,ramTotalBytes-(Number(freeHeap)||0));const usedKiB=Math.round(used/1024);const totalKiB=Math.round(ramTotalBytes/1024);const pct=Math.max(0,Math.min(100,used*100/ramTotalBytes));return usedKiB+'KiB/'+totalKiB+'KiB RAM used ('+pct.toFixed(1)+'%)'}
function openHowTo(){const dialog=document.getElementById('howto_dialog');if(dialog&&typeof dialog.showModal==='function')dialog.showModal()}
function closeHowTo(){const dialog=document.getElementById('howto_dialog');if(dialog&&typeof dialog.close==='function')dialog.close()}
function setSetupStep(id,done,text){const el=document.getElementById(id);if(!el)return;el.className='setup-step '+(done?'done':'wait');const state=el.querySelector('.setup-state');if(state)state.textContent=text}
function updateSetupStatus(s){const tapUp=!!s.tap_link_up;setSetupStep('setup_tap',tapUp,tapUp?'Connected':'No TAP response');const polling=!!s.polling_enabled;setSetupStep('setup_polling',polling,polling?'Enabled':'Enable polling');const confirmed=Number(s.node_confirmed_count??s.node_count??0);const expected=Number(s.network_expected_nodes||s.panel_field_count||0);const mapped=panelMapCache.filter(x=>String(x.long_addr||'').trim().length>0).length;const nodesReady=confirmed>0&&(expected===0||confirmed>=expected);setSetupStep('setup_nodes',nodesReady,confirmed+' confirmed'+(expected?' / '+expected:'')+(mapped?' | '+mapped+' mapped':''));const power=Number(s.power_count||0);setSetupStep('setup_power',power>0,power>0?power+' reporting':'Waiting for optimizer reports')}
function queuedFetch(task){const run=fetchQueue.then(task,task);fetchQueue=run.catch(()=>{});return run}
async function j(u,o){return queuedFetch(async()=>{const c=new AbortController();const t=setTimeout(()=>c.abort(),3500);try{const r=await fetch(u,Object.assign({cache:'no-store',signal:c.signal},o||{}));return r.json()}finally{clearTimeout(t)}})}
function esc(s){return String(s||'').replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;')}
function fmtDuration(totalSeconds){const s=Math.max(0,Math.floor(totalSeconds));const d=Math.floor(s/86400);const h=Math.floor((s%86400)/3600);const m=Math.floor((s%3600)/60);const sec=s%60;let out='';if(d>0)out+=d+'d ';if(d>0||h>0)out+=h+'h ';if(d>0||h>0||m>0)out+=m+'m ';out+=sec+'s';return out.trim()}
function sortNodes(nodes){return [...nodes].sort((a,b)=>(a.node_id||0)-(b.node_id||0))}
function nodeOptionText(node){let out='Node '+String(node.node_id||'?');if(node.long_addr)out+=' | '+String(node.long_addr);if(node.panel_label)out+=' | assigned '+String(node.panel_label);return out}
function updateLiveCounters(){const now=Date.now();const uptimeMs=uptimeBaseMs>0?(uptimeBaseMs+(now-uptimeSyncMs)):0;document.getElementById('uptime_value').textContent=uptimeMs>0?(fmtDuration(uptimeMs/1000)+' ('+Math.floor(uptimeMs/1000)+' s)'):'-'}
function setLiveFrameLine(text,ok){const frameLine=document.getElementById('last_frame_line');frameLine.className='frame-line '+(ok?'frame-ok':'frame-bad');frameLine.textContent=text||'No TAP frame received yet.';frameLine.title=frameLine.textContent}
  function updateLastFrameLineDisplay(s){if(s.last_frame_live_line){setLiveFrameLine(s.last_frame_live_line,!!s.last_frame_crc_ok);return}const frameCounter=s.frames_rx||0;setLiveFrameLine('msg #'+String(frameCounter)+' | tap responses '+String(s.tap_responses_rx||0)+' | poll timeouts '+String(s.poll_timeouts||0)+' | last type '+String(s.last_frame_type_code_hex||'-')+' | command '+String(s.command_state||'-'),!!s.last_frame_crc_ok)}
  function showCommandNotice(result,successText){const notice=document.getElementById('command_notice');if(!notice)return;const ok=!!(result&&result.ok);let text=successText||String((result&&result.message)||'Command accepted.');if(!ok){const message=String((result&&result.message)||'');text=message==='command busy'?'Another TAP command is still running. Please wait a moment.':(message||'The command could not be accepted.')}notice.textContent=text;notice.className='command-notice show'+(ok?'':' error');clearTimeout(commandNoticeTimer);commandNoticeTimer=setTimeout(()=>{notice.className='command-notice'},10000)}
  async function refreshLiveFrame(){try{const f=await j('/api/live-frame');setLiveFrameLine(f.line,!!f.crc_ok);updateTapLinkDisplay(f)}catch(_e){}}
async function loadMqttSettings(){try{const m=await j('/api/mqtt-settings');document.getElementById('mqtt_host').value=m.host||'';document.getElementById('mqtt_port').value=m.port||1883;document.getElementById('mqtt_base_topic').value=m.base_topic||'';document.getElementById('mqtt_username').value=m.username||'';document.getElementById('mqtt_password').value='';document.getElementById('mqtt_settings_status').textContent='Client '+(m.client_id||'-')+' | connected '+(m.connected?'yes':'no')+' | password '+(m.password_set?'set':'empty')}catch(_e){document.getElementById('mqtt_settings_status').textContent='MQTT settings could not be loaded.'}}
  async function saveMqttSettings(){const params=new URLSearchParams();params.append('host',(document.getElementById('mqtt_host').value||'').trim());params.append('port',(document.getElementById('mqtt_port').value||'1883').trim());params.append('base_topic',(document.getElementById('mqtt_base_topic').value||'').trim());params.append('username',(document.getElementById('mqtt_username').value||'').trim());const password=document.getElementById('mqtt_password').value;if(password.length>0)params.append('password',password);const r=await j('/api/mqtt-settings/save',{method:'POST',body:params});showCommandNotice(r,'MQTT settings saved. Reconnecting...');setTimeout(loadMqttSettings,500);setTimeout(refresh,500)}
  async function rebootDevice(){if(!confirm('Reboot ESP now?'))return;const r=await j('/api/reboot',{method:'POST'});showCommandNotice(r,'ESP restart scheduled.')}
function updateTapLinkDisplay(s){const box=document.getElementById('tap_link_box');const up=!!s.tap_link_up;box.className='tap-link '+(up?'tap-link-ok':'tap-link-bad');const age=s.tap_link_age_ms||0;box.textContent=up?'TAP connection: OK ('+String(age)+' ms)':'TAP connection: NO RESPONSE'}
function formatTapVersionText(raw){const text=String(raw||'').trim().replace(/\s+/g,' ');if(!text)return '-';const m=text.match(/^(Mgate Version \S+)\s+([A-Z][a-z]{2}\s+\d{1,2}\s+\d{4})\s+(\d{2}:\d{2}:\d{2})\s+(GW-\S+)/);if(m)return m[1]+'\nBuild: '+m[2]+' '+m[3]+'\nGateway: '+m[4];return text}
function updateTapVersionDisplay(s){const box=document.getElementById('tap_version_box');box.textContent='TAP version:\n'+formatTapVersionText(s.version_text)}
function onOptimizerSelectChange(index){const select=document.getElementById('optimizer_select_'+index);const input=document.getElementById('optimizer_'+index);if(!select||!input)return;if(select.value==='__manual__')return;input.value=select.value;configDirty=true}
function bindConfigInputs(){const panelCountInput=document.getElementById('panel_count');panelCountInput.oninput=()=>{configDirty=true;renderConfigRows()};document.querySelectorAll('#config_rows input').forEach(el=>{el.oninput=()=>{configDirty=true}});document.querySelectorAll('#config_rows select').forEach(el=>{el.onchange=()=>{onOptimizerSelectChange(el.dataset.index)}})}
function renderConfigRows(){const desired=parseInt(document.getElementById('panel_count').value||'1',10)||1;const count=Math.max(1,Math.min(maxOptimizers,desired));let rows='';for(let i=0;i<count;i++){const item=panelMapCache[i]||{label:'A'+(i+1),long_addr:''};const current=String(item.long_addr||'').toUpperCase();const selectId='optimizer_select_'+(i+1);const inputId='optimizer_'+(i+1);const hasDetected=discoveredNodesCache.some(node=>String(node.long_addr||'').toUpperCase()===current);let options='<option value=""'+(current?'':' selected')+'>Unassigned</option>';for(const node of discoveredNodesCache){const longAddr=String(node.long_addr||'').toUpperCase();const selected=longAddr&&longAddr===current?' selected':'';options+='<option value="'+esc(longAddr)+'"'+selected+'>'+esc(nodeOptionText(node))+'</option>'}options+='<option value="__manual__"'+(!hasDetected&&current?' selected':'')+'>Manual entry below</option>';rows+='<tr><td>'+(i+1)+'</td><td>'+esc(item.label)+'</td><td><label for="'+selectId+'">Discovered optimizer</label><select data-index="'+(i+1)+'" id="'+selectId+'">'+options+'</select><label for="'+inputId+'" style="margin-top:8px">Assigned long address</label><input id="'+inputId+'" placeholder="04C05B4000..." value="'+esc(item.long_addr||'')+'"></td></tr>'}document.getElementById('config_rows').innerHTML=rows;bindConfigInputs()}
async function refreshHeavyData(){try{const n=await j('/api/node-map');discoveredNodesCache=sortNodes(n.nodes||[]);document.getElementById('nodes').textContent=JSON.stringify(n,null,2);if(!configDirty){const pm=await j('/api/panel-map');panelMapCache=pm.panel_map||[];maxOptimizers=Math.max(1,pm.max_optimizers||1);const panelCountInput=document.getElementById('panel_count');panelCountInput.max=String(maxOptimizers);panelCountInput.value=pm.panel_field_count||1;renderConfigRows()}const p=await j('/api/power');document.getElementById('power_rows').innerHTML=(p.power||[]).map(x=>'<tr><td>'+esc(x.panel_label||'')+'</td><td>'+x.node_id+'</td><td>'+esc(x.short_addr_hex||'')+'</td><td>'+esc(x.long_addr||'')+'</td><td>'+(x.power??x.power_in_w??'-')+'</td><td>'+x.temp_c+'</td><td>'+x.rssi+'</td><td>'+x.age_ms+'</td><td>'+x.fresh+'</td></tr>').join('');const e=await j('/api/events');document.getElementById('events').textContent=(e.events||[]).map(x=>x.ms+' '+x.text).join('\n');if(window.lastStatus)updateSetupStatus(window.lastStatus)}catch(_e){}}
  async function saveConfig(){const params=new URLSearchParams();const count=Math.max(1,Math.min(maxOptimizers,parseInt(document.getElementById('panel_count').value||'1',10)||1));params.append('panel_count',String(count));for(let i=1;i<=count;i++){params.append('optimizer_'+i,(document.getElementById('optimizer_'+i)?.value||'').trim().toUpperCase())}const r=await j('/api/panel-map/save',{method:'POST',body:params});configDirty=false;showCommandNotice(r,'Panel mapping saved.');setTimeout(refreshHeavyData,400);setTimeout(refresh,400)}
  async function requestNodeTable(){const r=await j('/api/command/node-table');showCommandNotice(r,'Node table request sent. Waiting for TAP response.');setTimeout(refreshHeavyData,600);setTimeout(refresh,600)}
  async function rewritePvConfig(){if(!confirm('Schedule reporting config for all known optimizers? A TAP acknowledgement alone does not prove RF delivery.'))return;const r=await j('/api/command/rewrite-pv-config');showCommandNotice(r,'Reporting configuration scheduled. Checking TAP response...');setTimeout(refresh,400);setTimeout(refreshHeavyData,1200)}
  async function releaseOptimizers(){if(!confirm('Send the verified RSD RUN command to the TAP? The inverter can take about 60 seconds to start.'))return;const params=new URLSearchParams({mode:'run',confirm:'RSD_RUN'});const r=await j('/api/command/rsd-control',{method:'POST',body:params});showCommandNotice(r,'RSD RUN accepted. Allow about 60 seconds for inverter startup.');setTimeout(refresh,500)}
  async function setPolling(enabled){const r=await j('/api/polling/set?enabled='+(enabled?'1':'0'));showCommandNotice(r,enabled?'TAP polling enabled.':'TAP polling paused.');setTimeout(refresh,400)}
function togglePolling(){const input=document.getElementById('polling_toggle');setPolling(!!(input&&input.checked))}
async function refresh(){if(refreshBusy)return;refreshBusy=true;try{const s=await j('/api/status');window.lastStatus=s;uptimeBaseMs=s.uptime_ms||0;uptimeSyncMs=Date.now();updateLiveCounters();document.getElementById('gateway_id_value').textContent=s.gateway_id_hex||'-';document.getElementById('next_packet_value').textContent=s.next_packet_hex||'-';document.getElementById('ip_value').textContent=s.ip||'-';document.getElementById('wifi_mode_value').textContent=s.wifi_mode||'-';document.getElementById('hostname_value').textContent=s.hostname||'-';document.getElementById('mqtt_value').textContent=s.mqtt_connected?'yes':'no';document.getElementById('free_heap_value').textContent=(s.free_heap!=null)?formatRamUsage(s.free_heap):'-';document.getElementById('gateway_long_addr_value').textContent=s.gateway_long_addr||'-';document.getElementById('node_count_value').textContent=String(s.node_count||0);document.getElementById('power_count_value').textContent=String(s.power_count||0);document.getElementById('message_counter_value').textContent=String(s.frames_rx||0);document.getElementById('frames_crc_error_value').textContent=String(s.frames_crc_error||0);document.getElementById('polls_sent_value').textContent=String(s.polls_sent||0);document.getElementById('poll_timeouts_value').textContent=String(s.poll_timeouts||0);document.getElementById('polling_value').textContent=s.polling_enabled?'enabled':'disabled';document.getElementById('polling_toggle').checked=!!s.polling_enabled;document.getElementById('panel_field_count_value').textContent=String(s.panel_field_count||0);document.getElementById('firmware_version_value').textContent=s.firmware_version||'-';document.getElementById('version_text_value').textContent=s.version_text||'-';document.getElementById('live_sum_input_w_value').textContent=String(s.live_sum_input_w??'-');document.getElementById('held_sum_input_w_value').textContent=String(s.held_sum_input_w??'-');document.getElementById('fresh_nodes_value').textContent=String(s.fresh_nodes??'-');document.getElementById('stale_nodes_value').textContent=String(s.stale_nodes??'-');document.getElementById('expired_nodes_value').textContent=String(s.expired_nodes??'-');document.getElementById('newest_sample_age_ms_value').textContent=String(s.newest_sample_age_ms??'-');document.getElementById('oldest_sample_age_ms_value').textContent=String(s.oldest_sample_age_ms??'-');document.getElementById('avg_sample_age_ms_value').textContent=String(s.avg_sample_age_ms??'-');document.getElementById('rsd_control_state_value').textContent=s.rsd_control_state||'unknown';document.getElementById('command_name_value').textContent=s.command_name||'-';document.getElementById('command_state_value').textContent=s.command_state||'-';updateTapLinkDisplay(s);updateTapVersionDisplay(s);updateLastFrameLineDisplay(s);updateSetupStatus(s)}catch(_e){}finally{refreshBusy=false}}
refresh();
loadMqttSettings();
refreshHeavyData();
setInterval(refresh,10000);
setInterval(()=>{if(!configDirty)refreshHeavyData()},15000);
setInterval(refreshLiveFrame,2000);
setInterval(()=>{updateLiveCounters();if(window.lastStatus){updateTapLinkDisplay(window.lastStatus)}},1000);
)JS";

#endif
