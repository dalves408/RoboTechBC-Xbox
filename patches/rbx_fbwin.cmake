# Robotech: narrow edits to the toolkit's framebuffer window (src/video/fb_present.c), without modifying the toolkit.
#
# 1. Focus. xbox_FramebufferRaiseFocus pulled the game in front of whatever window had focus: every 3 s for the
#    first two minutes (main.c, for bat launches whose console kept focus) and again whenever an FMV ended. In a
#    window that stole focus from other programs over and over. It now only takes focus back from our own launch
#    console (or when nothing has focus), which is the case it was written for.
# 2. Title bar after fullscreen. Leaving fullscreen restored the rectangle saved on entry. When the game starts
#    fullscreen (settings.ini fullscreen=1) that rectangle could already be monitor-sized, so the window came back
#    as large as the screen with its title bar off the top. It now comes back at most the work area's size, with
#    its title bar on screen.
if(WIN32)
    set(_rbx_fbwin_input "${XBOXRECOMP_DIR}/src/video/fb_present.c")
    set(_rbx_fbwin_output "${CMAKE_CURRENT_BINARY_DIR}/rbx_fb_present.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rbx_fbwin_input}")
    file(READ "${_rbx_fbwin_input}" _rbx_fbwin)

    function(rbx_fbwin_replace before after)
        string(FIND "${_rbx_fbwin}" "${before}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "XboxRecomp framebuffer window changed. Review patches/rbx_fbwin.cmake.")
        endif()
        string(REPLACE "${before}" "${after}" _rbx_fbwin "${_rbx_fbwin}")
        set(_rbx_fbwin "${_rbx_fbwin}" PARENT_SCOPE)
    endfunction()

    rbx_fbwin_replace([=[    if (!s_fb_hwnd)
        return;
    ShowWindow(s_fb_hwnd, SW_SHOW);
    BringWindowToTop(s_fb_hwnd);
    fg = GetForegroundWindow();]=]
        [=[    if (!s_fb_hwnd)
        return;
    fg = GetForegroundWindow();
    {
        /* Robotech: only take focus back from our own launch console (or from nobody); never pull the game in
         * front of another program the player switched to. */
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        if (fg && fg != GetConsoleWindow() && fg_pid != GetCurrentProcessId())
            return;
    }
    ShowWindow(s_fb_hwnd, SW_SHOW);
    BringWindowToTop(s_fb_hwnd);]=])

    rbx_fbwin_replace([=[        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);]=]
        [=[        {
            /* Robotech: come back at most the work area's size, with the title bar on screen. Starting
             * fullscreen could save a monitor-sized rectangle. */
            MONITORINFO mi;
            LONG w = saved.right - saved.left, ht = saved.bottom - saved.top;
            mi.cbSize = sizeof mi;
            if (!(saved_style & WS_CAPTION))
                saved_style = (saved_style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
            if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
                RECT wa = mi.rcWork;
                LONG ww = wa.right - wa.left, wh = wa.bottom - wa.top;
                if (w <= 0 || ht <= 0 || w >= ww || ht >= wh) {
                    RECT r = { 0, 0, (LONG)s_fb_width * 2, (LONG)s_fb_height * 2 };
                    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                    w = r.right - r.left; ht = r.bottom - r.top;
                    if (w > ww || ht > wh) { w = ww * 3 / 4; ht = wh * 3 / 4; }
                    saved.left = wa.left + (ww - w) / 2; saved.top = wa.top + (wh - ht) / 2;
                }
                if (saved.top < wa.top) saved.top = wa.top;
                if (saved.left < wa.left) saved.left = wa.left;
                if (saved.left + w > wa.right) saved.left = wa.right - w;
                if (saved.top + ht > wa.bottom) saved.top = wa.bottom - ht;
                saved.right = saved.left + w; saved.bottom = saved.top + ht;
            }
        }
        SetWindowLongPtrA(h, GWL_STYLE, saved_style);
        SetWindowPos(h, NULL, saved.left, saved.top, saved.right - saved.left,
                     saved.bottom - saved.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);]=])

    file(CONFIGURE OUTPUT "${_rbx_fbwin_output}" CONTENT "${_rbx_fbwin}" @ONLY)
    get_target_property(_rbx_video_sources xbox_video SOURCES)
    list(FILTER _rbx_video_sources EXCLUDE REGEX "(^|/)fb_present[.]c$")
    set_property(TARGET xbox_video PROPERTY SOURCES ${_rbx_video_sources})
    target_sources(xbox_video PRIVATE "${_rbx_fbwin_output}")
    target_include_directories(xbox_video PRIVATE "${XBOXRECOMP_DIR}/src/video")
endif()
