/* www_style.h: the page's CSS, and the document head that carries it.
 *
 * One of the three pieces www_page.h concatenates. Split out because the
 * stylesheet shares nothing with the markup or the script: a colour tweak
 * should not mean scrolling past the event handlers, and a diff of one should
 * not touch the others.
 *
 * WWW_STYLE runs from the doctype to `</head>`, so everything a browser reads
 * before the first element is in one file. `<body>` opens WWW_BODY.
 *
 * Single quotes on every HTML attribute, as everywhere in this page, so no C
 * escaping is needed. The backslash at the end of each line continues the
 * #define; the string literals concatenate exactly as they did when this was
 * one literal in www_page.h, which is what keeps the served bytes identical.
 */
#ifndef AMTX_WWW_STYLE_H
#define AMTX_WWW_STYLE_H

#define WWW_STYLE \
"<!doctype html><html lang='en'><head><meta charset='utf-8'>" \
"<meta name='viewport' content='width=device-width,initial-scale=1'>" \
"<title>AM Transmitter</title><style>" \
":root{--bg:#12141a;--panel:#1b1e26;--line:#2b303c;--fg:#e6e8ee;--dim:#9aa2b1;" \
"--accent:#ffb454;--accent2:#7fb8ff;--ok:#6fcf7f;--bad:#e5657a}" \
"*{box-sizing:border-box}" \
"body{margin:0;background:var(--bg);color:var(--fg);" \
"font:14px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}" \
"header{padding:12px 18px;border-bottom:1px solid var(--line);" \
"display:flex;align-items:baseline;gap:16px;flex-wrap:wrap}" \
"h1{margin:0;font-size:16px;letter-spacing:.06em;text-transform:uppercase}" \
"header .car{font-variant-numeric:tabular-nums;font-size:13px}" \
".cA{color:var(--accent)}.cB{color:var(--accent2)}" \
"nav{display:flex;gap:2px;padding:0 12px;border-bottom:1px solid var(--line);" \
"overflow-x:auto}" \
"nav button{background:none;border:0;color:var(--dim);padding:10px 14px;" \
"font:inherit;cursor:pointer;border-bottom:2px solid transparent;white-space:nowrap}" \
"nav button.on{color:var(--fg);border-bottom-color:var(--accent)}" \
"#sel{display:flex;gap:8px;padding:10px 18px;align-items:center;" \
"border-bottom:1px solid var(--line);background:#161922;flex-wrap:wrap}" \
"#sel .lbl{color:var(--dim);font-size:12px;text-transform:uppercase;" \
"letter-spacing:.06em}" \
"#sel button{background:none;border:1px solid var(--line);color:var(--dim);" \
"border-radius:5px;padding:6px 14px;font:inherit;cursor:pointer}" \
"#sel button.on{color:#191919;font-weight:600}" \
"#sel button.on.a{background:var(--accent);border-color:var(--accent)}" \
"#sel button.on.b{background:var(--accent2);border-color:var(--accent2)}" \
"main{padding:18px;max-width:760px}" \
"section{display:none}section.on{display:block}" \
".card{background:var(--panel);border:1px solid var(--line);border-radius:8px;" \
"padding:14px;margin-bottom:14px}" \
".card.a{border-left:3px solid var(--accent)}" \
".card.b{border-left:3px solid var(--accent2)}" \
".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:10px}" \
".row label{flex:0 0 140px;color:var(--dim)}" \
"input,select{background:#0e1015;border:1px solid var(--line);color:var(--fg);" \
"border-radius:5px;padding:7px 9px;font:inherit;min-width:0;flex:1}" \
"input[type=range]{padding:0}" \
"button.act{background:var(--accent);border:0;color:#191919;border-radius:5px;" \
"padding:8px 14px;font:inherit;font-weight:600;cursor:pointer;flex:0 0 auto}" \
"button.sec{background:none;border:1px solid var(--line);color:var(--fg);" \
"border-radius:5px;padding:7px 11px;font:inherit;cursor:pointer;flex:0 0 auto}" \
"button.sec:hover{border-color:var(--accent)}" \
\
/* Press feedback. :active covers mouse and keyboard; the .pressed class is \
 * added by script on pointerdown because a tap on a phone can start and end \
 * faster than :active is ever painted. */ \
"button{transition:transform .06s ease,filter .06s ease,background .12s ease}" \
"button:active,button.pressed{transform:translateY(1px) scale(.985)}" \
"button.act:active,button.act.pressed{filter:brightness(.84)}" \
"button.sec:active,button.sec.pressed{background:#272c38;border-color:var(--accent)}" \
"nav button:active,nav button.pressed{color:var(--fg);background:#20242e}" \
"button[disabled]{opacity:.55;cursor:progress;transform:none}" \
\
/* Acknowledgement: a toast for routine actions, a dialog for the ones the \
 * operator has to read, which means anything that reboots the board. */ \
"#toast{position:fixed;left:50%;bottom:22px;z-index:50;max-width:92vw;" \
"transform:translateX(-50%) translateY(28px);opacity:0;pointer-events:none;" \
"background:var(--panel);color:var(--fg);border:1px solid var(--line);" \
"border-left:4px solid var(--ok);border-radius:8px;padding:10px 16px;" \
"box-shadow:0 8px 28px #0009;transition:opacity .18s ease,transform .18s ease}" \
"#toast.on{opacity:1;transform:translateX(-50%) translateY(0)}" \
"#toast.bad{border-left-color:var(--bad)}" \
"#ovl{position:fixed;inset:0;z-index:60;background:#000b;padding:18px;" \
"display:none;align-items:center;justify-content:center}" \
"#ovl.on{display:flex}" \
"#dlg{background:var(--panel);border:1px solid var(--line);border-radius:10px;" \
"padding:18px;width:100%;max-width:430px;box-shadow:0 12px 44px #000b}" \
"#dlg h2{margin:0 0 10px;font-size:14px;letter-spacing:.05em;" \
"text-transform:uppercase;color:var(--accent)}" \
"#dlgb{color:var(--dim);margin-bottom:16px}" \
"#dlgb b{color:var(--fg)}" \
\
".st{display:flex;align-items:center;gap:8px;padding:8px 10px;border-radius:6px;" \
"border:1px solid transparent}" \
".st:hover{background:#20242e}" \
".st.cur{border-color:var(--accent)}" \
".st[draggable]{cursor:grab}" \
".st.drag{opacity:.45;cursor:grabbing}" \
".st.over{border-color:var(--accent);background:#20242e;" \
"box-shadow:inset 0 2px 0 var(--accent)}" \
"button.star{background:none;border:1px solid var(--line);border-radius:5px;" \
"padding:7px 10px;font:inherit;cursor:pointer;flex:0 0 auto;color:var(--dim)}" \
"button.star.on{color:var(--accent);border-color:var(--accent)}" \
".grip{color:var(--dim);cursor:grab;padding:0 2px;user-select:none}" \
".st .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}" \
".st .u{display:block;color:var(--dim);font-size:11px}" \
".meter{height:9px;background:#0e1015;border-radius:5px;overflow:hidden}" \
".meter i{display:block;height:100%;background:var(--ok);width:0;" \
"transition:width .12s linear}" \
".meter i.hot{background:var(--accent)}.meter i.clip{background:var(--bad)}" \
"table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}" \
"td{padding:4px 0;border-bottom:1px solid var(--line)}" \
"td:last-child{text-align:right;color:var(--dim)}" \
".warn{border-left:3px solid var(--accent);padding-left:10px;color:var(--dim);" \
"font-size:12px;margin-top:12px}" \
".bad{color:var(--bad)}.ok{color:var(--ok)}.dim{color:var(--dim)}" \
\
/* Portrait on a touch screen: pointer:coarse keeps a narrow desktop window \
 * from being told to turn sideways, which it cannot do. */ \
"#rot{display:none;align-items:center;gap:10px;padding:10px 18px;" \
"background:#2a2213;border-bottom:1px solid var(--accent);font-size:13px}" \
"#rot b{color:var(--accent)}#rot .ico{font-size:20px}" \
"#rot button{margin-left:auto;background:none;border:1px solid var(--line);" \
"color:var(--fg);border-radius:5px;padding:4px 10px;font:inherit;cursor:pointer}" \
"@media (orientation:portrait) and (pointer:coarse){#rot{display:flex}}" \
"#rot.gone{display:none!important}" \
"</style></head>"

#endif /* AMTX_WWW_STYLE_H */
