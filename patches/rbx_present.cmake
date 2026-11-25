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

    file(CONFIGURE OUTPUT "${_rbx_present_output}" CONTENT "${_rbx_present}" @ONLY)
    get_target_property(_rbx_video_sources xbox_video SOURCES)
    list(FILTER _rbx_video_sources EXCLUDE REGEX "(^|/)gpu_present[.]c$")
    set_property(TARGET xbox_video PROPERTY SOURCES ${_rbx_video_sources})
    target_sources(xbox_video PRIVATE "${_rbx_present_output}")
    target_include_directories(xbox_video PRIVATE "${XBOXRECOMP_DIR}/src/video")
endif()
