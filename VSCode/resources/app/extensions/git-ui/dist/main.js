!function(e,t){for(var r in t)e[r]=t[r]}(exports,function(e){var t={};function r(n){if(t[n])return t[n].exports;var o=t[n]={i:n,l:!1,exports:{}};return e[n].call(o.exports,o,o.exports,r),o.l=!0,o.exports}return r.m=e,r.c=t,r.d=function(e,t,n){r.o(e,t)||Object.defineProperty(e,t,{enumerable:!0,get:n})},r.r=function(e){"undefined"!=typeof Symbol&&Symbol.toStringTag&&Object.defineProperty(e,Symbol.toStringTag,{value:"Module"}),Object.defineProperty(e,"__esModule",{value:!0})},r.t=function(e,t){if(1&t&&(e=r(e)),8&t)return e;if(4&t&&"object"==typeof e&&e&&e.__esModule)return e;var n=Object.create(null);if(r.r(n),Object.defineProperty(n,"default",{enumerable:!0,value:e}),2&t&&"string"!=typeof e)for(var o in e)r.d(n,o,function(t){return e[t]}.bind(null,o));return n},r.n=function(e){var t=e&&e.__esModule?function(){return e.default}:function(){return e};return r.d(t,"a",t),t},r.o=function(e,t){return Object.prototype.hasOwnProperty.call(e,t)},r.p="",r(r.s=0)}([function(e,t,r){"use strict";Object.defineProperty(t,"__esModule",{value:!0});const n=r(1),o=r(2);function i(e,t={}){return new Promise((r,n)=>{const i=o.exec(e,t,(e,t,o)=>{(e?n:r)({error:e,stdout:t,stderr:o})});t.stdin&&i.stdin.write(t.stdin,e=>{e?n(e):i.stdin.end(e=>{e&&n(e)})})})}t.deactivate=async function(){},t.activate=async function(e){e.subscriptions.push(n.commands.registerCommand("git.credential",async e=>{try{const{stdout:t,stderr:r}=await i(`git credential ${e.command}`,{stdin:e.stdin,env:Object.assign(process.env,{GIT_TERMINAL_PROMPT:"0"})});return{stdout:t,stderr:r,code:0}}catch({stdout:e,stderr:t,error:r}){const n=r.code||0;return-1!==t.indexOf("terminal prompts disabled")&&(t=""),{stdout:e,stderr:t,code:n}}}))},t.exec=i},function(e,t){e.exports=require("vscode")},function(e,t){e.exports=require("child_process")}]));
//# sourceMappingURL=https://ticino.blob.core.windows.net/sourcemaps/26076a4de974ead31f97692a0d32f90d735645c0/extensions/git-ui/dist/main.js.map