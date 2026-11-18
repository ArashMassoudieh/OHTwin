// Keep historical charts visible while clearly distinguishing the new calibration.
(() => {
    const notice = document.createElement('div');
    notice.setAttribute('role', 'status');
    notice.style.cssText = 'position:fixed;bottom:8px;left:12px;right:12px;z-index:10000;padding:10px 14px;border-radius:8px;background:#fff3cd;color:#513d08;font:14px/1.4 sans-serif;box-shadow:0 2px 10px #0002';
    notice.textContent = 'Fitness/parameters: previous calibration. Comparison: current observations and provisional model output. Updated MB + shallow A0/A2 calibration is running; results switch automatically when ready.';
    document.body.appendChild(notice);
    async function check() {
        try {
            const response = await fetch('outputs/calibration_shallow_v1/ga_output_merged.txt', {method:'HEAD', cache:'no-store'});
            if (response.ok) { window.location.replace('latest/OHTwinViewer.html'); return; }
        } catch (_) { /* Preserve the historical view if the server is unavailable. */ }
        window.setTimeout(check, 60000);
    }
    check();
})();
