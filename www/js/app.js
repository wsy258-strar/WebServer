/**
 * webserver - 客户端交互脚本
 * 演示静态 JS 文件的缓存服务
 */
(function () {
    'use strict';

    // 页面加载完成后执行
    document.addEventListener('DOMContentLoaded', function () {
        console.log('🚀 webserver 静态资源加载成功');
        console.log('✅ LFU 缓存已生效');
        console.log('✅ 内存池已初始化');

        // 卡片渐入动画
        var cards = document.querySelectorAll('.card');
        cards.forEach(function (card, i) {
            card.style.opacity = '0';
            card.style.transform = 'translateY(20px)';
            card.style.transition = 'opacity 0.4s ease, transform 0.4s ease';
            card.style.transitionDelay = (i * 0.1) + 's';

            // 触发重排后添加动画
            requestAnimationFrame(function () {
                card.style.opacity = '1';
                card.style.transform = '';
            });
        });
    });
})();
