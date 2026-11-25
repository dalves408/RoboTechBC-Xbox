if(WIN32)
    set(_rbx_present_input "${XBOXRECOMP_DIR}/src/video/gpu_present.c")
    set(_rbx_present_output "${CMAKE_CURRENT_BINARY_DIR}/rbx_gpu_present.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rbx_present_input}")
    file(READ "${_rbx_present_input}" _rbx_present)

    # Apply narrow edits to the shared presenter without modifying the toolkit.
    function(rbx_present_replace before after)
        string(FIND "${_rbx_present}" "${before}" _at)
        if(_at EQUAL -1)
            message(FATAL_ERROR "XboxRecomp presenter changed. Review the Robotech presenter patch.")
        endif()
        string(REPLACE "${before}" "${after}" _rbx_present "${_rbx_present}")
        set(_rbx_present "${_rbx_present}" PARENT_SCOPE)
    endfunction()

    rbx_present_replace([=[void gpu_present_set_source(void *shared_handle, uint32_t width, uint32_t height)
{
    s_src_handle = shared_handle;
    s_src_w = width;
    s_src_h = height;
    InterlockedIncrement(&s_src_gen);
}]=]
        [=[static SRWLOCK rbx_source_lock = SRWLOCK_INIT;

void gpu_present_set_source(void *shared_handle, uint32_t width, uint32_t height)
{
    AcquireSRWLockExclusive(&rbx_source_lock);
    s_src_handle = shared_handle;
    s_src_w = width;
    s_src_h = height;
    InterlockedIncrement(&s_src_gen);
    ReleaseSRWLockExclusive(&rbx_source_lock);
}]=])
    rbx_present_replace([=[    source_follow();
    use_src = s_src_srv != NULL;]=]
        [=[    if (s_src_gen != s_src_open_gen) {
        AcquireSRWLockShared(&rbx_source_lock);
        source_follow();
        ReleaseSRWLockShared(&rbx_source_lock);
    }
    use_src = s_src_srv != NULL;]=])

    rbx_present_replace([=[    if (use_src && FAILED(IDXGIKeyedMutex_AcquireSync(s_src_mutex, 0, 0)))
        return 0;                       /* the back end has it: the next pass shows it */]=]
        [=[    if (use_src) {
        extern void rbx_present_abandoned(void);
        extern int rbx_present_trace(void);
        HRESULT acquired = IDXGIKeyedMutex_AcquireSync(s_src_mutex, 0, 0);
        if (acquired == WAIT_ABANDONED)
            rbx_present_abandoned();
        if (acquired != S_OK) {
            if (rbx_present_trace())
                fprintf(stderr, "[RBX PRESENT] read gen=%ld serial=%ld acquire=%08lX\n",
                        (long)s_src_open_gen, (long)serial, (unsigned long)acquired);
            return 0;
        }
    }]=])

    rbx_present_replace([=[    static const char *path;
    char req[MAX_PATH], bmp[MAX_PATH];]=]
        [=[    static const char *path, *capture;
    static unsigned captures, every = 1;
    char req[MAX_PATH], bmp[MAX_PATH];]=])
    rbx_present_replace([=[    if (!checked) { checked = 1; path = getenv("RECOMP_PRESENT_SHOT"); }
    if (!path) return;
    snprintf(req, sizeof req, "%s.req", path);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return;
    snprintf(bmp, sizeof bmp, "%s.bmp", path);]=]
        [=[    if (!checked) {
        const char *interval = getenv("RECOMP_RBX_SWAPSHOT_EVERY");
        checked = 1;
        path = getenv("RECOMP_PRESENT_SHOT");
        capture = getenv("RECOMP_RBX_SWAPSHOT");
        if (interval && atoi(interval) > 0)
            every = (unsigned)atoi(interval);
    }
    req[0] = 0;
    if (path) {
        snprintf(req, sizeof req, "%s.req", path);
        if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES)
            req[0] = 0;
    }
    if (req[0])
        snprintf(bmp, sizeof bmp, "%s.bmp", path);
    else if (capture && *capture && captures++ % every == 0)
        snprintf(bmp, sizeof bmp, "%s_f%06u.bmp", capture, captures);
    else
        return;]=])
    rbx_present_replace([=[    DeleteFileA(req);]=] [=[    if (req[0]) DeleteFileA(req);]=])
    rbx_present_replace([=[    return IDXGISwapChain_Present(s_chain, (UINT)s_vsync, 0) == S_OK && s_vsync;]=]
        [=[    HRESULT result = IDXGISwapChain_Present(s_chain, (UINT)s_vsync, 0);
    extern int rbx_present_trace(void);
    if (rbx_present_trace())
        fprintf(stderr, "[RBX PRESENT] show gen=%ld serial=%ld shared=%d tex=%ux%u client=%ux%u present=%08lX\n",
                (long)s_src_open_gen, (long)serial, use_src, fw, fh, s_bw, s_bh, (unsigned long)result);
    return result == S_OK && s_vsync;]=])

    # A new shared frame wakes the window thread (see the fb_present.c patch below),
    # and the window thread can tell whether it has already copied the current flip.
    rbx_present_replace([=[void gpu_present_frame_ready(void) { InterlockedIncrement(&s_src_serial); }]=]
        [=[static void * volatile rbx_frame_event;

/* Auto-reset event set for every published GPU frame; the window loop waits on it. */
void *rbx_present_frame_event(void)
{
    if (!rbx_frame_event) {
        HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (event && InterlockedCompareExchangePointer((void * volatile *)&rbx_frame_event, event, NULL))
            CloseHandle(event);
    }
    return rbx_frame_event;
}

/* Nonzero while the window shows the render back end's shared frame, not s_rgb. */
int rbx_present_shared_active(void) { return s_src_srv != NULL; }

void gpu_present_frame_ready(void)
{
    InterlockedIncrement(&s_src_serial);
    if (rbx_present_frame_event())
        SetEvent((HANDLE)rbx_frame_event);
}]=])

    file(CONFIGURE OUTPUT "${_rbx_present_output}" CONTENT "${_rbx_present}" @ONLY)

    # The framebuffer window loop (fb_present.c). While the shared GPU frame is on screen, s_rgb is
    # only used for screenshots and dumps, so copy it once per flip instead of on every pass, and
    # sleep until the next published frame (or 16 ms, or a window message) instead of 1 ms.
    # fb_overlay still runs every pass: it hands PVIDEO banks back to the title.
    set(_rbx_fb_input "${XBOXRECOMP_DIR}/src/video/fb_present.c")
    set(_rbx_fb_output "${CMAKE_CURRENT_BINARY_DIR}/rbx_fb_present.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rbx_fb_input}")
    file(READ "${_rbx_fb_input}" _rbx_present)
    rbx_present_replace([=[            if (s_present_idx >= 0 && s_present[s_present_idx]) {
                memcpy(s_rgb, s_present[s_present_idx],
                       (size_t)s_fb_width * s_fb_height * 4);]=]
        [=[            extern int rbx_present_shared_active(void);
            static LONG rbx_copied_flip = -1;
            LONG rbx_flip = s_flips;
            if (s_present_idx >= 0 && s_present[s_present_idx]) {
                if (!getenv("RECOMP_RBX_FB_COPY_ALWAYS") && rbx_present_shared_active() && rbx_flip == rbx_copied_flip) {
                    /* The window shows the GPU frame and this flip is already in s_rgb. */
                } else {
                    memcpy(s_rgb, s_present[s_present_idx],
                           (size_t)s_fb_width * s_fb_height * 4);
                    rbx_copied_flip = rbx_flip;
                }]=])
    rbx_present_replace([=[            Sleep(gpu_present_active() ? gpu_present_idle_ms() : 16);   /* D3D11's Present waited for vsync */]=]
        [=[        {
            extern void *rbx_present_frame_event(void);
            extern int rbx_present_shared_active(void);
            HANDLE frame = gpu_present_active() && rbx_present_shared_active() && !getenv("RECOMP_RBX_FB_SLEEP")
                           ? (HANDLE)rbx_present_frame_event() : NULL;
            if (frame)
                MsgWaitForMultipleObjects(1, &frame, FALSE, 16, QS_ALLINPUT);
            else
                Sleep(gpu_present_active() ? gpu_present_idle_ms() : 16);   /* D3D11's Present waited for vsync */
        }]=])
    file(CONFIGURE OUTPUT "${_rbx_fb_output}" CONTENT "${_rbx_present}" @ONLY)

    get_target_property(_rbx_video_sources xbox_video SOURCES)
    list(FILTER _rbx_video_sources EXCLUDE REGEX "(^|/)(gpu_present|fb_present)[.]c$")
    set_property(TARGET xbox_video PROPERTY SOURCES ${_rbx_video_sources})
    target_sources(xbox_video PRIVATE "${_rbx_present_output}" "${_rbx_fb_output}")
    target_include_directories(xbox_video PRIVATE "${XBOXRECOMP_DIR}/src/video")
endif()
