/* www_script.h: the page's JavaScript.
 *
 * One of the three pieces www_page.h concatenates. Split out because it is the
 * only executable part, and the only part that can be checked at all without a
 * browser: pulled out of the literal it runs through `node --check`, which is
 * worth something for code no compiler in this build ever looks at.
 *
 * WWW_SCRIPT is `<script>` through `</script></body></html>`, so it closes the
 * document. It reaches www_body.h only through element ids, and the board only
 * through the /api routes in webui.c.
 *
 * JavaScript strings use single quotes so the C literal needs no escaping,
 * except where an HTML attribute built in JavaScript needs double quotes and
 * those are escaped. The backslash at the end of each line continues the
 * #define; the string literals concatenate exactly as they did when this was
 * one literal in www_page.h.
 */
#ifndef AMTX_WWW_SCRIPT_H
#define AMTX_WWW_SCRIPT_H

#define WWW_SCRIPT \
"<script>" \
"var S={},D=[],toastT=0,cur=0,touched=false,hostTouched=false;" \
"var NAMES=['A','B','C','D'];" \
"function $(i){return document.getElementById(i)}" \
"function v(i){return $(i).value}" \
"function sh(i,t){$(i).textContent=t}" \
"function esc(s){var d=document.createElement('div');d.textContent=s||'';" \
"return d.innerHTML}" \
"function C(){return (S.chains&&S.chains[cur])||null}" \
"function nm(i){return NAMES[i]||('#'+i)}" \
\
"function toast(m,bad){var t=$('toast');t.textContent=m;" \
"t.className='on'+(bad?' bad':'');clearTimeout(toastT);" \
"toastT=setTimeout(function(){t.className=''},2800)}" \
/* With `act` given, the dialog asks: the safe choice is the highlighted \
 * Cancel, and the action is the plain button beside it. */ \
"function modal(h,b,act,fn){$('dlgh').innerHTML=h;$('dlgb').innerHTML=b;" \
"var a=$('dlgact');$('dlgok').textContent=act?'Cancel':'OK';" \
"a.style.display=act?'':'none';a.textContent=act||'';" \
"a.onclick=function(){closeModal();if(fn)fn()};" \
"$('ovl').className='on'}" \
"function hostName(){return (S.sys&&S.sys.host)||'amtx'}" \
"function rotGone(){$('rot').classList.add('gone');" \
"try{sessionStorage.setItem('amtxRot','1')}catch(x){}}" \
"try{if(sessionStorage.getItem('amtxRot'))$('rot').classList.add('gone')}catch(x){}" \
"function closeModal(){$('ovl').className=''}" \
\
"document.addEventListener('pointerdown',function(e){" \
"var b=e.target&&e.target.closest?e.target.closest('button'):null;if(!b)return;" \
"b.classList.add('pressed');" \
"setTimeout(function(){b.classList.remove('pressed')},180)});" \
\
/* Every POST carries the selected chain unless the caller names one, and \
 * acknowledges: the reply is {ok:bool,error:string}. */ \
"function post(u,b,msg){" \
"if(b&&b.ch===undefined)b.ch=cur;" \
"return fetch(u,{method:'POST',headers:{'Content-Type':'application/json'}," \
"body:JSON.stringify(b)})" \
".then(function(r){return r.json().catch(function(){return {ok:r.ok}})})" \
".then(function(j){" \
"if(j&&j.ok===false)toast((msg||'Command')+' rejected: '+(j.error||'error'),1);" \
"else toast(msg||'Done');" \
"setTimeout(poll,250);return j})" \
".catch(function(){toast((msg||'Command')+': no reply from the transmitter',1)})}" \
\
"document.querySelectorAll('nav button').forEach(function(b){" \
"b.onclick=function(){" \
"document.querySelectorAll('nav button').forEach(function(x){x.classList.remove('on')});" \
"document.querySelectorAll('section').forEach(function(x){x.classList.remove('on')});" \
"b.classList.add('on');$(b.dataset.t).classList.add('on')}});" \
\
"function pick(i){cur=i;touched=false;" \
"document.querySelectorAll('#sel button').forEach(function(x){" \
"x.classList.toggle('on',+x.dataset.c===i)});" \
"$('rfcard').className='card '+(i?'b':'a');" \
"toast('Now acting on transmitter '+nm(i));" \
"if(S.chains)render(S)}" \
\
"document.querySelectorAll('#rf input,#rf select').forEach(function(e){" \
"e.addEventListener('input',function(){touched=true});" \
"e.addEventListener('change',function(){touched=true})});" \
\
"function poll(){fetch('/api/state').then(function(r){return r.json()})" \
".then(function(s){S=s;render(s)}).catch(function(){})}" \
\
"function hdr(el,c,i){" \
"if(!c){$(el).textContent='';return}" \
"$(el).innerHTML='<b class=c'+nm(i)+'>'+nm(i)+'</b> '" \
"+(c.rf.on?'ON AIR ':'OFF ')+(c.rf.hz/1000).toFixed(1)+' kHz '+c.rf.depth+'%';" \
"$(el).className='car'+(c.rf.on?'':' dim')}" \
\
"function render(s){" \
"if(!s.chains)return;" \
"if(cur>=s.chains.length)cur=0;" \
"hdr('hdrA',s.chains[0],0);hdr('hdrB',s.chains[1],1);" \
"var c=C();if(!c)return;" \
"sh('selhint','Stations, Discover and Transmitter act on '+nm(cur));" \
"renderNow(s);" \
"if(!touched){$('hz').value=c.rf.hz;$('depth').value=c.rf.depth;" \
"$('lpf').value=c.rf.lpf;$('gain').value=c.rf.gain;" \
"$('level').value=c.rf.level;$('on').value=c.rf.on?'1':'0';" \
"$('agc').value=c.rf.agc?'1':'0';$('agct').value=c.rf.agct;" \
"$('fdm').value=c.rf.fdm;$('fdr').value=c.rf.fdr;$('fdd').value=c.rf.fdd;" \
"sh('depthv',c.rf.depth+'%');sh('gainv',c.rf.gain+'%');" \
"sh('levelv',c.rf.level+'%');sh('agctv',c.rf.agct+'%');" \
"sh('fdrv',(c.rf.fdr/1000).toFixed(2)+' Hz');sh('fddv',c.rf.fdd+'%')}" \
/* These two always update: they report what the board is doing, not what the \
 * operator typed, so a slider being dragged must not freeze them. */ \
"sh('agcnow',c.rf.agc?('now '+(c.audio.agcdb>=0?'+':'')" \
"+c.audio.agcdb.toFixed(1)+' dB'):'off');" \
"sh('fdnow',c.rf.fdm?('level now '+c.rf.levelnow+'%'):'');" \
"renderStations(s);renderNet(s);renderDiag(s)}" \
\
/* Both transmitters at once: what is on air is the question you want answered \
 * without clicking anything. */ \
"function renderNow(s){var h='';" \
"s.chains.forEach(function(c,i){" \
"var p=Math.round(c.audio.peak*100);" \
"h+='<div class=\"card '+(i?'b':'a')+'\">'" \
"+'<div class=dim style=\"font-size:12px;letter-spacing:.06em\">TRANSMITTER '" \
"+nm(i)+' &middot; '+(c.rf.hz/1000).toFixed(1)+' kHz &middot; '" \
"+(c.rf.on?'carrier on':'carrier off')+'</div>'" \
"+'<div style=\"font-size:18px;margin:4px 0\">'+esc(c.play.name||'Nothing playing')" \
"+' <span class=dim style=font-size:12px>'+esc(c.play.state)+'</span></div>'" \
"+'<div class=dim style=\"font-size:12px;word-break:break-all\">'" \
"+esc(c.play.url||'')+'</div>'" \
"+'<div style=\"margin:12px 0 6px\" class=dim>Modulation</div>'" \
"+'<div class=meter><i class=\"'+(p>97?'clip':(p>80?'hot':''))" \
"+'\" style=\"width:'+p+'%\"></i></div>'" \
"+'<div class=row style=\"margin-top:14px\">'" \
"+'<button class=act onclick=\"post(&quot;/api/stop&quot;,{ch:'+i+'},&quot;Stopped&quot;)\">Stop</button>'" \
"+'<button class=sec onclick=\"rfToggle('+i+')\">'" \
"+(c.rf.on?'Carrier off':'Carrier on')+'</button>'" \
"+'</div></div>'});" \
"$('nowcards').innerHTML=h}" \
\
"function renderStations(s){var h='';var c=C();" \
"if(!s.stations.length)h='<div class=dim>No stations saved yet.</div>';" \
"s.stations.forEach(function(t,i){" \
"var isDef=t.def&&t.def[cur];" \
"h+='<div class=\"st'+(c&&i==c.play.index?' cur':'')+'\" draggable=true'" \
"+' ondragstart=\"dragStart(event,'+i+')\" ondragover=\"dragOver(event)\"'" \
"+' ondragleave=\"dragLeave(event)\" ondrop=\"dropOn(event,'+i+')\"'" \
"+' ondragend=\"dragEnd(event)\">'" \
"+'<span class=grip title=\"Drag to reorder\">&#8942;&#8942;</span>'" \
"+'<div class=nm>'+esc(t.name)+'<span class=u>'+esc(t.url)+'</span></div>'" \
"+'<button class=\"star'+(isDef?' on':'')+'\" title=\"power-on station\"'" \
"+' onclick=\"setDefault('+i+','+(isDef?1:0)+')\">&#9733;</button>'" \
"+'<button class=sec onclick=\"playIdx('+i+')\">Play</button>'" \
"+'<button class=sec title=\"Level trim for this station\"'" \
"+' onclick=\"trimSta('+i+','+(t.gain||100)+')\">'+(t.gain||100)+'%</button>'" \
"+'<button class=sec onclick=\"post(&quot;/api/station/move&quot;,{from:'+i+',to:'+(i-1)+'},&quot;Moved up&quot;)\"'+(i==0?' disabled':'')+'>&uarr;</button>'" \
"+'<button class=sec onclick=\"post(&quot;/api/station/move&quot;,{from:'+i+',to:'+(i+1)+'},&quot;Moved down&quot;)\"'+(i==s.stations.length-1?' disabled':'')+'>&darr;</button>'" \
"+'<button class=sec onclick=\"post(&quot;/api/station/delete&quot;,{index:'+i+'},&quot;Station deleted&quot;)\">&times;</button>'" \
"+'</div>'});" \
"$('stalist').innerHTML=h}" \
\
"function renderNet(s){" \
"$('netstat').innerHTML='<table>'" \
"+'<tr><td>State</td><td>'+esc(s.net.state)+'</td></tr>'" \
"+'<tr><td>Network</td><td>'+esc(s.net.ssid||'-')+'</td></tr>'" \
"+'<tr><td>Address</td><td>'+esc(s.net.ip)+'</td></tr>'" \
"+'<tr><td>Hostname</td><td>'+esc(s.sys.host)+'.local</td></tr>'" \
"+'<tr><td>Signal</td><td>'+(s.net.rssi?s.net.rssi+' dBm':'-')+'</td></tr>'" \
"+'<tr><td>Drops</td><td>'+s.net.drops+'</td></tr></table>';" \
"if(!hostTouched)$('host').value=s.sys.host||''}" \
\
"function rrow(k,val,cls){return '<tr><td>'+k+'</td><td class=\"'+(cls||'')" \
"+'\">'+val+'</td></tr>'}" \
\
"function renderDiag(s){var h='';" \
"s.chains.forEach(function(c,i){" \
"h+='<div class=\"card '+(i?'b':'a')+'\">'" \
"+'<div class=dim style=\"font-size:12px;letter-spacing:.06em;margin-bottom:6px\">'" \
"+'TRANSMITTER '+nm(i)+'</div><table>'" \
"+rrow('Stream buffer',Math.round(100*c.stream.buffered/c.stream.capacity)+'%  ('" \
"+c.stream.buffered+' B)')" \
"+rrow('HTTP status',c.stream.status||'-')" \
"+rrow('Reconnects',c.stream.reconnects,c.stream.reconnects?'bad':'')" \
"+rrow('Last error',esc(c.stream.error||'none'),c.stream.error?'bad':'ok')" \
"+rrow('Decoded frames',c.audio.frames)" \
"+rrow('Decode errors',c.audio.errors,c.audio.errors?'bad':'')" \
"+rrow('Resyncs',c.audio.resyncs)" \
"+rrow('Stream format',c.audio.rate+' Hz  '+c.audio.ch+' ch  '+c.audio.kbps+' kbit/s')" \
"+rrow('Resampler trim',c.audio.trim+' ppm')" \
"+rrow('Modulator FIFO',c.rf.fifo+' / '+c.rf.fifocap+' samples')" \
"+rrow('Modulator underruns',c.rf.underruns,c.rf.underruns?'bad':'ok')" \
"+'</table></div>'});" \
"$('diagcards').innerHTML=h;" \
"$('sysdiag').innerHTML=" \
"rrow('Free heap, total',Math.round(s.sys.heap/1024)+' kB')" \
"+rrow('Free heap, internal',Math.round(s.sys.heapint/1024)+' kB'" \
"+' (largest block '+Math.round(s.sys.heapintmax/1024)+' kB)'," \
"s.sys.heapint<40000?'bad':'')" \
"+rrow('Uptime',Math.floor(s.sys.uptime/60)+' min')" \
"+rrow('Firmware',esc(s.sys.version))}" \
\
/* Reordering reuses /api/station/move: a drag from i to j is exactly the \
 * splice that endpoint already performs, so there is nothing new to store. \
 * The arrows stay because HTML5 drag-and-drop does not work on touch. */ \
"var dragFrom=-1;" \
"function dragStart(e,i){dragFrom=i;" \
"try{e.dataTransfer.effectAllowed='move';" \
"e.dataTransfer.setData('text/plain',String(i))}catch(x){}" \
"e.currentTarget.classList.add('drag')}" \
"function dragOver(e){if(dragFrom<0)return;e.preventDefault();" \
"try{e.dataTransfer.dropEffect='move'}catch(x){}" \
"e.currentTarget.classList.add('over')}" \
"function dragLeave(e){e.currentTarget.classList.remove('over')}" \
"function dropOn(e,i){e.preventDefault();" \
"e.currentTarget.classList.remove('over');" \
"var f=dragFrom;dragFrom=-1;" \
"if(f<0||f==i)return;" \
"post('/api/station/move',{from:f,to:i},'Moved to position '+(i+1))}" \
"function dragEnd(e){dragFrom=-1;" \
"document.querySelectorAll('.st').forEach(function(x){" \
"x.classList.remove('drag','over')})}" \
\
"function playIdx(i){post('/api/station/play',{index:i}," \
"'Playing on '+nm(cur))}" \
\
"function setDefault(i,isDef){" \
"if(isDef)post('/api/station/default',{index:-1}," \
"'Power-on station cleared for '+nm(cur));" \
"else post('/api/station/default',{index:i},'Plays on '+nm(cur)+' at power-on')}" \
\
"function playUrl(){var u=v('adhoc').trim();" \
"if(!u){toast('Enter a stream URL first',1);return}" \
"post('/api/station/play',{url:u},'Playing on '+nm(cur))}" \
\
"function saveRf(){post('/api/rf',{on:v('on')=='1',hz:+v('hz')," \
"depth:+v('depth'),lpf:+v('lpf'),gain:+v('gain'),level:+v('level')," \
"agc:v('agc')=='1',agct:+v('agct'),fdm:+v('fdm'),fdr:+v('fdr'),fdd:+v('fdd')}," \
"nm(cur)+(v('on')=='1'?' saved, carrier on':' saved, carrier off'));" \
"touched=false}" \
\
"function trimSta(i,now){" \
"var t=prompt('Level trim for this station, percent (10 to 400). '" \
"+'The AGC handles most of the difference between stations; this is for when '" \
"+'it gets one wrong.',now);" \
"if(t===null)return;t=parseInt(t,10);" \
"if(!(t>=10&&t<=400)){toast('Trim must be between 10 and 400',1);return}" \
"post('/api/station/gain',{index:i,gain:t},'Trim set to '+t+'%')}" \
\
"function rfToggle(i){var c=S.chains&&S.chains[i];if(!c)return;var n=!c.rf.on;" \
"post('/api/rf',{ch:i,on:n,hz:c.rf.hz,depth:c.rf.depth," \
"lpf:c.rf.lpf,gain:c.rf.gain},nm(i)+(n?' carrier on':' carrier off'))}" \
\
"function tone(on){post('/api/tone',{on:!!on,hz:+v('tonehz')}," \
"nm(cur)+(on?' test tone on at '+v('tonehz')+' Hz':' test tone off'))}" \
\
"function addSta(){var n=v('sname').trim(),u=v('surl').trim();" \
"if(!n||!u){toast('A station needs both a name and a URL',1);return}" \
"post('/api/station/add',{name:n,url:u},'Station added')" \
".then(function(j){if(j&&j.ok!==false){$('sname').value='';$('surl').value=''}})}" \
\
"function eye(){var p=$('pass'),b=$('eyeb');" \
"if(p.type=='password'){p.type='text';b.innerHTML='&#128065; Hide';" \
"b.setAttribute('aria-label','Hide password')}" \
"else{p.type='password';b.innerHTML='&#128065; Show';" \
"b.setAttribute('aria-label','Show password')}}" \
\
/* The board reboots to join, which kills this connection. Say so plainly: a \
 * silent disconnect here reads as a failure when it is the success path. */ \
"function addWifi(){var s=v('ssid').trim(),b=$('joinb');" \
"if(!s){toast('Pick or type a network name first',1);return}" \
"b.disabled=true;b.textContent='Saving...';" \
"fetch('/api/wifi/add',{method:'POST'," \
"headers:{'Content-Type':'application/json'}," \
"body:JSON.stringify({ssid:s,pass:v('pass')})})" \
".then(function(r){return r.json()})" \
".then(function(j){" \
"if(!j||j.ok===false){b.disabled=false;b.textContent='Save and join';" \
"modal('Not saved','The transmitter rejected those details: <b>'" \
"+esc((j&&j.error)||'unknown')+'</b>.');return}" \
"b.textContent='Saved';" \
"modal('Saved &mdash; joining '+esc(s)," \
"'The transmitter has saved the credentials and is <b>restarting now</b> to" \
" join <b>'+esc(s)+'</b>.<br><br>" \
"This setup portal is about to disconnect. That is expected: the AMTX-Setup" \
" access point has to come down before the transmitter can join your" \
" network.<br><br>" \
"Reconnect this device to <b>'+esc(s)+'</b>, then open" \
" <b>http://'+esc(hostName())+'.local</b>.<br><br>" \
"If the password was wrong it gives up after about 15 seconds and brings" \
" <b>AMTX-Setup</b> back, so you can rejoin here and try again.')})" \
".catch(function(){b.disabled=false;b.textContent='Save and join';" \
"modal('No reply'," \
"'The transmitter did not answer. If it accepted the details it is already" \
" restarting: reconnect to your own network and try" \
" <b>http://'+esc(hostName())+'.local</b>.')})}" \
\
"function reboot(){post('/api/reboot',{},'Rebooting');" \
"modal('Rebooting','The transmitter is restarting. This page will go quiet for" \
" a few seconds, then reconnect by itself.')}" \
\
"function setHost(){var h=v('host').trim();" \
"if(!/^[A-Za-z0-9]([A-Za-z0-9-]{0,29}[A-Za-z0-9])?$/.test(h)){" \
"toast('Letters, digits and hyphens, up to 31, not starting or ending with a hyphen',1);" \
"return}" \
"post('/api/hostname',{host:h},'Now at http://'+h.toLowerCase()+'.local')" \
".then(function(j){if(j&&j.ok!==false)hostTouched=false})}" \
\
/* Asks first, and says plainly that the page cannot undo this: once the \
 * server stops there is nothing here to press. */ \
"function webOff(){modal('Turn off the web interface?'," \
"'This page stops working in a moment and <b>cannot turn itself back on</b>." \
" Only the serial console can: connect a USB cable and type <b>web on</b>." \
" With the web off there is no setup portal either, so a board that loses" \
" its network is joined again from the console.','Turn it off',function(){" \
"post('/api/web',{on:false},'Web interface turning off').then(function(j){" \
"if(j&&j.ok!==false)modal('Web interface off','The transmitter has stopped" \
" serving this page. It keeps playing. Use the serial console, and" \
" <b>web on</b> to bring the page back.')})})}" \
\
/* Asks twice over: a dialog here, and `confirm` in the body, because the \
 * board cannot undo this and neither can the person. */ \
"function factoryReset(){modal('Erase everything saved?'," \
"'The transmitter forgets <b>every station</b>, <b>every saved Wi-Fi" \
" network</b>, both transmitters settings, its hostname and this page being" \
" on. It restarts as if it were new, so it will raise the <b>AMTX-Setup</b>" \
" access point and you will set it up again from scratch. The firmware" \
" stays.','Erase everything',function(){" \
"post('/api/factory/reset',{confirm:true},'Erasing, and restarting')" \
".then(function(j){if(j&&j.ok!==false)modal('Erasing','The transmitter is" \
" erasing what it had and restarting. This page will not come back: join" \
" <b>AMTX-Setup</b> to set it up again.')})})}" \
\
"function scan(){var b=$('scanb');b.disabled=true;b.textContent='Scanning...';" \
"$('scanlist').innerHTML='<div class=dim>Scanning...</div>';" \
"fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(l){" \
"b.disabled=false;b.textContent='Scan for networks';" \
"var h='';l.forEach(function(n){" \
"h+='<div class=st><div class=nm>'+esc(n.ssid)+'<span class=u>'+n.rssi" \
"+' dBm'+(n.secure?'  secured':'  open')+'</span></div>'" \
"+'<button class=sec onclick=\"useSsid('" \
"+JSON.stringify(n.ssid).replace(/\"/g,'&quot;')+')\">Use</button></div>'});" \
"$('scanlist').innerHTML=h||'<div class=dim>Nothing found.</div>';" \
"toast(l.length?l.length+' network'+(l.length==1?'':'s')+' found'" \
":'No networks found',l.length?0:1)})" \
".catch(function(){b.disabled=false;b.textContent='Scan for networks';" \
"$('scanlist').innerHTML='<div class=bad>Scan failed.</div>';" \
"toast('Scan failed',1)})}" \
\
"function useSsid(s){$('ssid').value=s;toast('Network set to '+s);" \
"$('pass').focus()}" \
\
/* Discovery is a blocking HTTPS round trip on the board, so the button is \
 * disabled while it runs rather than letting a second search queue. */ \
"function discover(){var q=v('dq').trim(),b=$('dbtn');" \
"if(!q){toast('Type something to search for',1);return}" \
"b.disabled=true;b.textContent='Searching...';" \
"$('dlist').innerHTML='<div class=dim>Searching the directory...</div>';" \
"fetch('/api/discover?by='+encodeURIComponent(v('dby'))" \
"+'&q='+encodeURIComponent(q))" \
".then(function(r){return r.json()}).then(function(l){" \
"b.disabled=false;b.textContent='Search';" \
"if(l&&l.error){$('dlist').innerHTML='<div class=bad>'+esc(l.error)+'</div>';" \
"toast('Search failed: '+l.error,1);return}" \
"D=l||[];var h='';" \
"D.forEach(function(n,i){" \
"h+='<div class=st><div class=nm>'+esc(n.name)+'<span class=u>'" \
"+esc(n.country||'unknown')+(n.bitrate?'  '+n.bitrate+' kbit/s':'')" \
"+(n.votes?'  '+n.votes+' votes':'')+'</span></div>'" \
"+'<button class=sec onclick=\"dplay('+i+')\">Play</button>'" \
"+'<button class=sec onclick=\"dsave('+i+')\">Save</button></div>'});" \
"$('dlist').innerHTML=h||'<div class=dim>Nothing found.</div>';" \
"toast(D.length?D.length+' station'+(D.length==1?'':'s')+' found'" \
":'Nothing found',D.length?0:1)})" \
".catch(function(){b.disabled=false;b.textContent='Search';" \
"$('dlist').innerHTML='<div class=bad>Search failed.</div>';" \
"toast('Search failed',1)})}" \
\
"function dplay(i){var n=D[i];if(!n)return;" \
"post('/api/station/play',{url:n.url},'Playing '+n.name+' on '+nm(cur))}" \
"function dsave(i){var n=D[i];if(!n)return;" \
"post('/api/station/add',{name:n.name,url:n.url},'Saved '+n.name)}" \
\
"poll();setInterval(poll,1000);" \
"</script></body></html>"

#endif /* AMTX_WWW_SCRIPT_H */
