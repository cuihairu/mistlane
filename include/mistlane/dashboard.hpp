// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>
namespace mistlane {
// The embedded operations dashboard: one self-contained page served by the
// node's --dashboard-listen listener. Zero external resources — no fonts, no
// scripts, no images — so an air-gapped host renders it exactly like a
// workstation. The only data source is the same Prometheus text the
// --metrics-listen exposer serves: this page fetches /metrics on its own
// origin every two seconds and renders connections, business rooms, message
// throughput and request/event-loop timing from it. Deliberately read-only
// and registry-backed, so the page can never disagree with a scraper.
inline constexpr std::string_view kDashboardHtml = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>mistlane dashboard</title>
<style>
:root{color-scheme:dark;--bg:#0d1117;--panel:#161b22;--edge:#30363d;--fg:#e6edf3;--dim:#8b949e;--ok:#3fb950;--warn:#d29922;--err:#f85149;--acc:#58a6ff}
*{box-sizing:border-box;margin:0}
body{background:var(--bg);color:var(--fg);font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:20px}
header{display:flex;align-items:baseline;gap:12px;margin-bottom:16px}
h1{font-size:17px;font-weight:600}
#role{color:var(--acc)}#stamp{color:var(--dim);font-size:12px;margin-left:auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}
.panel{background:var(--panel);border:1px solid var(--edge);border-radius:8px;padding:12px 14px}
.panel h2{font-size:11px;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin-bottom:8px}
.big{font-size:28px;font-weight:600}
.sub{color:var(--dim);font-size:12px;margin-top:2px}
.ok{color:var(--ok)}.warn{color:var(--warn)}.err{color:var(--err)}
table{width:100%;border-collapse:collapse;font-size:12px}
td{padding:2px 0}td:last-child{text-align:right}
svg{display:block;width:100%;height:48px}
#lost{display:none;margin-bottom:12px;border:1px solid var(--err);color:var(--err);border-radius:8px;padding:8px 14px}
footer{color:var(--dim);font-size:11px;margin-top:16px}
</style></head><body>
<header><h1>mistlane dashboard</h1><span id="role"></span><span id="stamp"></span></header>
<div id="lost">metrics unreachable — retrying</div>
<div class="grid">
  <div class="panel"><h2>Connections</h2><div class="big" id="conn">–</div>
    <div class="sub" id="connmax"></div><svg id="spark-conn" viewBox="0 0 100 32" preserveAspectRatio="none"></svg></div>
  <div class="panel"><h2>Rooms</h2><div class="big" id="rooms">–</div><div class="sub">script gauge mistlane_script_gauge</div></div>
  <div class="panel"><h2>Messages / s</h2><div class="big" id="rate">–</div>
    <div class="sub" id="bytes"></div><svg id="spark-rate" viewBox="0 0 100 32" preserveAspectRatio="none"></svg></div>
  <div class="panel"><h2>Request latency</h2><div class="big" id="lat">–</div><div class="sub" id="latn"></div></div>
  <div class="panel"><h2>Event loop</h2><div class="big" id="lag">–</div><div class="sub" id="rss"></div></div>
  <div class="panel"><h2>Entity results</h2><table id="entity"></table></div>
</div>
<footer>single-page, zero external resources · data: GET /metrics (Prometheus text) · refresh 2s</footer>
<script>
"use strict";
var histConn=[],histRate=[],lastTx=-1,lastRx=-1,lastStamp=0;
function fmt(n){return n>=1e6?(n/1e6).toFixed(1)+"M":n>=1e3?(n/1e3).toFixed(1)+"k":Math.round(n*100)/100+""}
// Minimal Prometheus text parser: name{labels} value pairs, comments dropped.
function parse(text){
  var out={},lines=text.split("\n");
  for(var i=0;i<lines.length;i++){
    var line=lines[i];
    if(!line||line[0]==="#")continue;
    var brace=line.indexOf("{"),space,value,key,name,labels="";
    if(brace>0&&(space=line.indexOf("}",brace))>0){
      key=line.slice(0,space+1);name=line.slice(0,brace);
      labels=line.slice(brace+1,space);value=parseFloat(line.slice(space+2));
    }else{
      space=line.lastIndexOf(" ");if(space<1)continue;
      key=line.slice(0,space);name=key;value=parseFloat(line.slice(space+1));
    }
    if(isNaN(value))continue;
    if(!out[name])out[name]=[];
    out[name].push({key:key,labels:labels,value:value});
  }
  return out;
}
function pick(m,name,label){
  var rows=m[name];if(!rows)return null;
  for(var i=0;i<rows.length;i++){
    var hit=!label;
    if(label&&rows[i].labels.indexOf(label)>=0)hit=true;
    if(hit)return rows[i].value;
  }
  return null;
}
function spark(id,series){
  var el=document.getElementById(id);if(!el)return;
  if(series.length<2){el.innerHTML="";return}
  var min=Math.min.apply(null,series),max=Math.max.apply(null,series);
  if(max===min)max=min+1;
  var pts="";
  for(var i=0;i<series.length;i++)
    pts+=(100*i/(series.length-1)).toFixed(2)+","+(30-(series[i]-min)/(max-min)*28).toFixed(2)+" ";
  el.innerHTML='<polyline points="'+pts+'" fill="none" stroke="#58a6ff" stroke-width="1.5"/>';
}
function render(m){
  var rs=m["mistlane_ready"];
  if(rs&&rs.length&&rs[0].labels){var mm=/role="([^"]+)"/.exec(rs[0].labels);document.getElementById("role").textContent=mm?"role "+mm[1]:"";}
  var conn=pick(m,"mistlane_active_sessions");
  document.getElementById("conn").textContent=conn===null?"–":fmt(conn);
  var cap=pick(m,"mistlane_max_connections");
  document.getElementById("connmax").textContent=cap!==null?"of "+fmt(cap)+" capacity":"";
  if(conn!==null){histConn.push(conn);if(histConn.length>60)histConn.shift();}
  spark("spark-conn",histConn);
  document.getElementById("rooms").textContent=(pick(m,"mistlane_script_gauge","rooms")??0)+"";
  var tx=pick(m,"mistlane_messages_total",'direction="tx"'),rx=pick(m,"mistlane_messages_total",'direction="rx"');
  var now=Date.now(),dt=(now-lastStamp)/1000;
  if(tx!==null&&lastTx>=0&&dt>0.2){
    var txr=(tx-lastTx)/dt,rxr=lastRx>=0&&rx!==null?(rx-lastRx)/dt:0;
    document.getElementById("rate").textContent=fmt(txr);
    document.getElementById("bytes").textContent="rx "+fmt(rxr)+" /s · "+fmt(tx)+" tx total";
    histRate.push(txr);if(histRate.length>60)histRate.shift();
    spark("spark-rate",histRate);
  }
  lastTx=tx;lastRx=rx;lastStamp=now;
  var sum=pick(m,"mistlane_request_seconds_sum"),count=pick(m,"mistlane_request_seconds_count");
  if(sum!==null&&count){document.getElementById("lat").textContent=(sum/count*1000).toFixed(2)+" ms";
    document.getElementById("latn").textContent=fmt(count)+" requests observed";}
  var lag=pick(m,"mistlane_event_loop_lag_max_seconds");
  var lagEl=document.getElementById("lag");
  if(lag!==null){lagEl.textContent=(lag*1000).toFixed(1)+" ms";lagEl.className="big "+(lag>1?"err":lag>0.2?"warn":"ok");}
  var rss=pick(m,"mistlane_process_resident_memory_bytes");
  document.getElementById("rss").textContent=rss!==null?"rss "+fmt(rss/1048576)+" MiB":"";
  var rows=m["mistlane_entity_results_total"]||[],body="";
  rows.sort(function(a,b){return b.value-a.value});
  for(var i=0;i<rows.length;i++){
    if(!rows[i].value)continue;
    var r=/result="([^"]+)"/.exec(rows[i].labels),name=r?r[1]:"?";
    var cls=name==="ok"||name==="notify"?"ok":name==="notify-error"||name==="invalid"?"err":"";
    body+="<tr><td>"+name+'</td><td class="'+cls+'">'+fmt(rows[i].value)+"</td></tr>";
  }
  document.getElementById("entity").innerHTML=body||"<tr><td>–</td></tr>";
}
function poll(){
  fetch("/metrics",{cache:"no-store"}).then(function(r){
    if(!r.ok)throw new Error(r.status);return r.text();
  }).then(function(text){
    document.getElementById("lost").style.display="none";
    render(parse(text));
    document.getElementById("stamp").textContent="updated "+new Date().toLocaleTimeString();
  }).catch(function(){
    document.getElementById("lost").style.display="block";
  });
}
poll();setInterval(poll,2000);
</script></body></html>)html";
}
