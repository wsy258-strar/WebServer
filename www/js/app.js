/**
 * AR 协同渲染 Demo — 会话管理
 * API:
 *   POST /api/auth?username=xxx&password=yyy          → 注册/登录合一，返回 session token
 *   POST /api/session/enter?token=xxx&scene=yyy       → 进入场景 (status=1, INSERT)
 *   POST /api/session/exit?token=xxx                   → 退出场景 (status=0, UPDATE)
 *   GET  /api/session?token=xxx                        → 查询会话
 */
(function () {
    'use strict';

    var currentToken = '';
    var currentUser = '';
    var logCount = 0;

    var SCENE_NAMES = {
        '1': '🏛️ 苔蚀巨神废墟',
        '2': '🛰️ 深空失联观测站',
        '3': '🍁 烟雨枫桥渡',
        '4': '🚇 灰霾地铁避难所',
        '5': '🌃 霓虹黑市深巷'
    };

    // ========== 工具 ==========
    function $(id) { return document.getElementById(id); }
    function show(el) { if (typeof el === 'string') el = $(el); el.classList.remove('hidden'); }
    function hide(el) { if (typeof el === 'string') el = $(el); el.classList.add('hidden'); }

    function addLog(type, msg) {
        logCount++;
        var list = $('log-list');
        var div = document.createElement('div');
        div.className = 'log-entry log-' + type;
        div.innerHTML = '<span class="log-num">#' + logCount + '</span> ' +
                        '<span class="log-time">' + new Date().toLocaleTimeString() + '</span> ' + msg;
        list.insertBefore(div, list.firstChild);
        if (list.children.length > 50) list.removeChild(list.lastChild);
        show('log-card');
    }

    function showResult(id, cls, msg) {
        var el = $(id);
        el.innerHTML = msg;
        el.className = 'result-box result-' + cls;
        show(el);
    }

    // ========== 认证（注册/登录合一） ==========
    window.doAuth = function () {
        var username = $('auth-username').value.trim();
        var password = $('auth-password').value.trim();
        if (!username) { showResult('auth-result', 'error', '请输入用户名'); return; }
        if (!password) { showResult('auth-result', 'error', '请输入密码'); return; }

        var start = performance.now();
        showResult('auth-result', 'info', '正在认证...');

        var url = '/api/auth?username=' + encodeURIComponent(username) +
                  '&password=' + encodeURIComponent(password);
        fetch(url, { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (data) {
                var elapsed = (performance.now() - start).toFixed(1);
                if (data.status === 'ok') {
                    currentToken = data.session_token;
                    currentUser  = data.username;

                    var action = data.is_new ? '注册+登录' : '登录';
                    showResult('auth-result', 'ok',
                        '✅ ' + action + '成功! <b>' + data.username + '</b> · ' +
                        elapsed + 'ms (MySQL ' + (data.is_new ? 'INSERT+SELECT' : 'SELECT') + ')');
                    addLog('ok', action + ': <b>' + data.username +
                           '</b> (token: ' + data.session_token.substr(0, 16) + '…, ' + elapsed + 'ms)');

                    // 显示会话区
                    $('sess-user').textContent = data.username;
                    $('sess-token').textContent = data.session_token;
                    show('session-card');
                    show('scene-card');

                    // 刷新会话信息
                    refreshSession();
                } else {
                    showResult('auth-result', 'error', '❌ ' + (data.error || '认证失败'));
                    addLog('error', '认证失败: ' + username);
                }
            })
            .catch(function () {
                showResult('auth-result', 'error', '❌ 网络错误: 服务器未启动或 MySQL 未连接');
            });
    };

    // ========== 查询会话 ==========
    function refreshSession() {
        if (!currentToken) return;
        fetch('/api/session?token=' + encodeURIComponent(currentToken))
            .then(function (r) { return r.json(); })
            .then(function (data) {
                if (data.status === 'ok') {
                    if (data.status_code == 1 && data.scene_id) {
                        $('sess-scene').textContent = SCENE_NAMES[data.scene_id] || data.scene_id;
                        $('sess-status').textContent = '🟢 活跃';
                        $('sess-status').className = 'status-badge badge-ok';
                    } else {
                        $('sess-scene').textContent = '(空)';
                        $('sess-status').textContent = '⚫ 已退出';
                        $('sess-status').className = 'status-badge badge-off';
                    }
                    $('sess-created').textContent = data.created_at || '-';
                    $('sess-updated').textContent = data.updated_at || '-';
                } else {
                    $('sess-scene').textContent = '无活跃会话';
                    $('sess-status').textContent = '未进入';
                    $('sess-status').className = 'status-badge badge-warn';
                }
            });
    }

    // ========== 进入场景 ==========
    window.enterScene = function () {
        var sceneId = $('scene-select').value;
        if (!sceneId) { showResult('scene-result', 'error', '请选择场景'); return; }
        if (!currentToken) { showResult('scene-result', 'error', '请先认证'); return; }

        var sceneName = SCENE_NAMES[sceneId] || sceneId;
        var start = performance.now();
        showResult('scene-result', 'info', '🔄 正在进入: <b>' + sceneName + '</b> ...');

        var url = '/api/session/enter?token=' + encodeURIComponent(currentToken) +
                  '&scene=' + encodeURIComponent(sceneId);
        fetch(url, { method: 'POST' })
            .then(function (r) {
                if (!r.ok) { throw new Error('HTTP ' + r.status); }
                return r.json();
            })
            .then(function (data) {
                var elapsed = (performance.now() - start).toFixed(1);
                if (data.status === 'ok') {
                    showResult('scene-result', 'ok',
                        '✅ 已进入 <b>' + sceneName + '</b> · ' +
                        elapsed + 'ms (UPDATE sessions, status=1)');
                    addLog('switch', '进入场景: <b>' + sceneName + '</b> (' + elapsed + 'ms)');
                    refreshSession();
                } else {
                    showResult('scene-result', 'error', '❌ ' + (data.error || '进入失败'));
                }
            })
            .catch(function (e) {
                showResult('scene-result', 'error', '❌ 请求失败: ' + (e.message || '网络错误'));
            });
    };

    // ========== 退出场景 ==========
    window.exitScene = function () {
        if (!currentToken) { showResult('scene-result', 'error', '请先认证'); return; }

        var start = performance.now();
        showResult('scene-result', 'info', '🔄 正在退出场景...');

        fetch('/api/session/exit?token=' + encodeURIComponent(currentToken), { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (data) {
                var elapsed = (performance.now() - start).toFixed(1);
                if (data.status === 'ok') {
                    showResult('scene-result', 'ok',
                        '✅ 已退出场景 · ' + elapsed + 'ms (UPDATE, scene_id清空, status=0)');
                    addLog('switch', '退出场景 (' + elapsed + 'ms)');
                    refreshSession();
                } else if (data.error === 'already exited') {
                    showResult('scene-result', 'error',
                        '⚠️ 当前未在场景中，无需重复退出');
                } else {
                    showResult('scene-result', 'error', '❌ ' + (data.error || '退出失败'));
                }
            })
            .catch(function () {
                showResult('scene-result', 'error', '❌ 网络错误');
            });
    };

    addLog('info', '页面加载完成，等待认证...');
})();
