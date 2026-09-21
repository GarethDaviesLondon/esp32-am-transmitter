/* www_body.h: the page's markup.
 *
 * One of the three pieces www_page.h concatenates. Split out because this is
 * the part a change to the interface usually lands in -- a new control, a
 * renamed tab -- and it is the part worth reading on its own.
 *
 * WWW_BODY opens `<body>` and ends just before `<script>`. Element ids are the
 * contract between this file and www_script.h: the script reaches everything
 * through $('<id>'), so renaming an id here breaks it there and nowhere else.
 *
 * The backslash at the end of each line continues the #define; the string
 * literals concatenate exactly as they did when this was one literal in
 * www_page.h.
 */
#ifndef AMTX_WWW_BODY_H
#define AMTX_WWW_BODY_H

#define WWW_BODY \
"<body>" \
\
"<header><h1>AM Transmitter</h1>" \
"<span class='car' id='hdrA'>&mdash;</span>" \
"<span class='car' id='hdrB'>&mdash;</span></header>" \
\
"<div id='rot' role='status'><span class='ico' aria-hidden='true'>&#128241;&#8635;</span>" \
"<span><b>Turn your phone sideways.</b> This page is laid out for landscape" \
" and is cramped held upright.</span>" \
"<button onclick='rotGone()' aria-label='Dismiss'>&times;</button></div>" \
\
"<nav>" \
"<button class='on' data-t='now'>Now playing</button>" \
"<button data-t='sta'>Stations</button>" \
"<button data-t='disc'>Discover</button>" \
"<button data-t='rf'>Transmitter</button>" \
"<button data-t='net'>Network</button>" \
"<button data-t='diag'>Diagnostics</button>" \
"</nav>" \
\
"<div id='sel'><span class='lbl'>Acting on</span>" \
"<button class='on a' data-c='0' onclick='pick(0)'>Transmitter A</button>" \
"<button class='b' data-c='1' onclick='pick(1)'>Transmitter B</button>" \
"<span class='dim' style='font-size:12px' id='selhint'></span></div>" \
\
"<main>" \
\
"<section id='now' class='on'><div id='nowcards'></div>" \
"<div class='card'><div class='dim' style='margin-bottom:8px'>" \
"Play a URL directly on the selected transmitter</div>" \
"<div class='row'><input id='adhoc' placeholder='http://stream.example/mp3'>" \
"<button class='act' onclick='playUrl()'>Play</button>" \
"</div></div></section>" \
\
"<section id='sta'><div class='card' id='stalist'></div>" \
"<div class='warn' style='margin:-6px 0 14px'>Drag a row to reorder, or use" \
" the arrows. Play and the star act on the selected transmitter; the star sets" \
" the station it tunes at power-on, and each transmitter has its own.</div>" \
"<div class='card'><div class='dim' style='margin-bottom:8px'>Add a station</div>" \
"<div class='row'><label>Name</label><input id='sname' placeholder='Radio 4 LW'></div>" \
"<div class='row'><label>Stream URL</label><input id='surl' placeholder='http://...'></div>" \
"<div class='row'><button class='act' onclick='addSta()'>Add</button></div>" \
"<div class='warn'>HTTP only. The stream client attaches no certificate" \
" bundle, so an https:// stream will not connect; Discover filters those out." \
"</div></div></section>" \
\
"<section id='disc'><div class='card'>" \
"<div class='dim' style='margin-bottom:8px'>Search the Radio-Browser directory</div>" \
"<div class='row'>" \
"<input id='dq' placeholder='jazz, classical, bbc ...'" \
" onkeydown='if(event.key==\"Enter\")discover()'>" \
"<select id='dby' style='flex:0 0 auto'>" \
"<option value='name'>by name</option>" \
"<option value='tag'>by genre</option>" \
"<option value='country'>by country</option></select>" \
"<button class='act' id='dbtn' onclick='discover()'>Search</button></div>" \
"<div id='dlist'></div>" \
"<div class='warn'>Results are MP3 only and http only, because that is what" \
" this firmware can actually play, and are ordered by how often people tune" \
" them. Play tries a station on the selected transmitter; Save adds it to the" \
" shared station list.</div></div></section>" \
\
"<section id='rf'><div class='card' id='rfcard'>" \
"<div class='row'><label>Carrier</label>" \
"<input id='hz' type='number' min='60000' max='300000' step='1000'>" \
"<span class='dim'>Hz</span></div>" \
"<div class='row'><label>Modulation depth</label>" \
"<input id='depth' type='range' min='5' max='90' oninput='sh(\"depthv\",this.value+\"%\")'>" \
"<span class='dim' id='depthv'></span></div>" \
"<div class='row'><label>Audio low-pass</label>" \
"<input id='lpf' type='number' min='500' max='9500' step='100'>" \
"<span class='dim'>Hz</span></div>" \
"<div class='row'><label>Programme gain</label>" \
"<input id='gain' type='range' min='0' max='400' oninput='sh(\"gainv\",this.value+\"%\")'>" \
"<span class='dim' id='gainv'></span></div>" \
"<div class='row'><label>Carrier level</label>" \
"<input id='level' type='range' min='5' max='100' oninput='sh(\"levelv\",this.value+\"%\")'>" \
"<span class='dim' id='levelv'></span></div>" \
"<div class='row'><label>Carrier output</label>" \
"<select id='on'><option value='1'>On</option><option value='0'>Off</option></select></div>" \
"<div class='row'><button class='act' onclick='saveRf()'>Apply and save</button></div>" \
"<div class='warn'>Carrier level is the honest way to turn the power down: it" \
" shrinks the whole radiated envelope. It costs resolution, because the" \
" envelope then uses fewer duty steps, so at 25% there is a quarter of the" \
" amplitude resolution there was at 100%.</div>" \
"<div class='warn'>Depth above 75% pinches the carrier at the trough and an" \
" envelope detector turns that into distortion. Both carriers must stay below" \
" about 312 kHz: the hardware divides an 80 MHz clock and needs 256 duty steps" \
" per cycle. Give each carrier its own output filter, and keep the field" \
" inside the room.</div></div>" \
"<div class='card'>" \
"<div class='row' style='margin-bottom:12px'>" \
"<label style='flex:0 0 auto'>Loudness AGC</label>" \
"<select id='agc' style='flex:0 0 auto'>" \
"<option value='1'>On</option><option value='0'>Off</option></select>" \
"<span class='dim' id='agcnow'></span></div>" \
"<div class='row'><label>Target level</label>" \
"<input id='agct' type='range' min='5' max='60' oninput='sh(\"agctv\",this.value+\"%\")'>" \
"<span class='dim' id='agctv'></span></div>" \
"<div class='row'><button class='act' onclick='saveRf()'>Apply and save</button></div>" \
"<div class='warn'>Stations differ by more than 10 dB in average level: a" \
" compressed music service runs near full scale, a jazz service with real" \
" dynamic range averages far lower, and speech with pauses lower still. The" \
" limiter cannot help, because it only pushes peaks down and never lifts quiet" \
" material up. This tracks the average and rides the gain, which is what" \
" broadcast AM processing does and why AM stations all sound equally loud." \
" It trades dynamic range for loudness. Turn it off if you would rather keep" \
" the dynamics.</div></div>" \
\
"<div class='card'>" \
"<div class='row' style='margin-bottom:12px'>" \
"<label style='flex:0 0 auto'>Fading</label>" \
"<select id='fdm' style='flex:0 0 auto'>" \
"<option value='0'>Off</option>" \
"<option value='1'>Slow fade</option>" \
"<option value='2'>Flutter</option>" \
"<option value='3'>Random drift</option></select>" \
"<span class='dim' id='fdnow'></span></div>" \
"<div class='row'><label>Rate</label>" \
"<input id='fdr' type='range' min='10' max='12000'" \
" oninput='sh(\"fdrv\",(this.value/1000).toFixed(2)+\" Hz\")'>" \
"<span class='dim' id='fdrv'></span></div>" \
"<div class='row'><label>Depth</label>" \
"<input id='fdd' type='range' min='0' max='100' oninput='sh(\"fddv\",this.value+\"%\")'>" \
"<span class='dim' id='fddv'></span></div>" \
"<div class='row'><button class='act' onclick='saveRf()'>Apply and save</button></div>" \
"<div class='warn'>Wanders the carrier level to imitate propagation. It acts on" \
" the carrier rather than the audio, so the whole signal comes and goes the way" \
" a distant station does, noise and all. Random drift sums three unrelated" \
" rates so it never repeats.</div></div>" \
\
"<div class='card'><div class='dim' style='margin-bottom:8px'>Test tone</div>" \
"<div class='row'><label>Frequency</label>" \
"<input id='tonehz' type='number' value='1000' min='50' max='5000'>" \
"<button class='sec' onclick='tone(1)'>Start</button>" \
"<button class='sec' onclick='tone(0)'>Stop</button></div>" \
"<div class='warn'>Replaces the programme on the selected transmitter with a" \
" steady tone at the modulator's own rate, so a scope on that RF pin shows the" \
" envelope with the network and the decoder taken out of the picture.</div>" \
"</div></section>" \
\
"<section id='net'><div class='card' id='netstat'></div>" \
"<div class='card'><div class='row'>" \
"<button class='sec' onclick='scan()' id='scanb'>Scan for networks</button></div>" \
"<div id='scanlist'></div>" \
"<div class='row'><label>Network</label><input id='ssid' placeholder='SSID'></div>" \
"<div class='row'><label>Password</label>" \
"<input id='pass' type='password' autocapitalize='none' autocorrect='off'" \
" spellcheck='false'>" \
"<button class='sec' id='eyeb' onclick='eye()' aria-label='Show password'" \
" title='Show the password so you can check it before joining'>" \
"&#128065; Show</button></div>" \
"<div class='row'><button class='act' id='joinb' onclick='addWifi()'>Save and join</button>" \
"<button class='sec' onclick='reboot()'>Reboot</button></div>" \
"<div class='warn'>Saving credentials restarts the transmitter, so this page" \
" loses contact for a moment. That is normal, and the dialog will say so.</div>" \
"</div>" \
"<div class='card'><div class='dim' style='margin-bottom:8px'>Hostname</div>" \
"<div class='row'><label>Name</label>" \
"<input id='host' placeholder='amtx' autocapitalize='none' autocorrect='off'" \
" spellcheck='false' oninput='hostTouched=true'>" \
"<span class='dim'>.local</span>" \
"<button class='act' onclick='setHost()'>Save</button></div>" \
"<div class='warn'>Give each transmitter on the network its own name: two" \
" called amtx both claim amtx.local, and only one answers. Letters, digits and" \
" hyphens, up to 31. The new name works at once; the router's device list" \
" catches up after a reboot.</div></div>" \
"<div class='card'><div class='dim' style='margin-bottom:8px'>Factory reset</div>" \
"<div class='row'><button class='sec' onclick='factoryReset()'>Erase everything" \
" and start again</button></div>" \
"<div class='warn'>Forgets every station, every saved network, both" \
" transmitters' settings, the hostname and this page's own on/off setting. The" \
" firmware stays. You can also hold the board's <b>BOOT</b> button for five" \
" seconds, which is the way back when this page cannot be reached at all.</div>" \
"</div>" \
"<div class='card'><div class='dim' style='margin-bottom:8px'>Web interface</div>" \
"<div class='row'><button class='sec' onclick='webOff()'>Turn off the web" \
" interface</button></div>" \
"<div class='warn'>This page has no password, so anyone on the network can" \
" retune the transmitter. Turning it off closes that. Once it is off, only the" \
" serial console can turn it back on, and there is no setup portal.</div>" \
"</div></section>" \
\
"<section id='diag'><div id='diagcards'></div>" \
"<div class='card'><table id='sysdiag'></table></div></section>" \
\
"</main>" \
\
"<div id='toast'></div>" \
"<div id='ovl'><div id='dlg'>" \
"<h2 id='dlgh'></h2><div id='dlgb'></div>" \
"<div class='row' style='margin:0'>" \
"<button class='act' id='dlgok' onclick='closeModal()'>OK</button>" \
"<button class='sec' id='dlgact' style='display:none'></button></div>" \
"</div></div>"


#endif /* AMTX_WWW_BODY_H */
