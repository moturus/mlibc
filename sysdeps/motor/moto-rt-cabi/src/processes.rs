//! Native children shared by all C-library spawn, wait and popen callers.
use alloc::{collections::BTreeMap, format, string::String, sync::Arc, vec::Vec};
use core::ffi::{CStr, c_char};
use core::sync::atomic::{AtomicI32, Ordering};
use moto_rt::{Error, Result, fs, mutex::Mutex, poll, process};

fn error(error: Error) -> i32 {
    crate::err64(error) as i32
}

struct NativeFd(i32);
impl Drop for NativeFd {
    fn drop(&mut self) {
        if self.0 >= 0 {
            fs::close(self.0).expect("owned libc descriptor");
        }
    }
}

struct Child {
    readiness: NativeFd,
    pid: i32,
    handle: u64,
    signal: AtomicI32,
}
impl Drop for Child {
    fn drop(&mut self) {
        process::wait(self.handle).expect("reaped child is complete");
        fs::close(core::mem::replace(&mut self.readiness.0, -1))
            .expect("owned child readiness descriptor");
        moto_rt::alloc::release_handle(self.handle).expect("owned child handle");
    }
}

struct Children {
    entries: BTreeMap<i32, Arc<Child>>,
    changed: Option<NativeFd>,
}
static CHILDREN: Mutex<Children> = Mutex::new(Children {
    entries: BTreeMap::new(),
    changed: None,
});

impl Children {
    fn notify(&self) {
        if let Some(changed) = &self.changed {
            poll::wake(changed.0).expect("live child-table notification");
        }
    }
}

#[repr(C)]
pub struct SpawnOptions {
    program: *const c_char,
    argv: *const *const c_char,
    envp: *const *const c_char,
    cwd: *const c_char,
    path: *const c_char,
    stdio: [i32; 3],
    flags: u32,
}

unsafe fn text<'a>(ptr: *const c_char) -> Result<&'a str> {
    if ptr.is_null() {
        return Err(Error::InvalidArgument);
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|_| Error::InvalidArgument)
}

unsafe fn strings(mut ptr: *const *const c_char) -> Result<Vec<String>> {
    let mut result = Vec::new();
    if !ptr.is_null() {
        while !unsafe { *ptr }.is_null() {
            result.push(unsafe { text(*ptr)? }.into());
            ptr = unsafe { ptr.add(1) };
        }
    }
    Ok(result)
}

fn join(base: &str, path: &str) -> String {
    if path.starts_with('/') {
        path.into()
    } else {
        format!("{base}/{path}")
    }
}

fn resolve(program: &str, cwd: &str, path: &str, search: bool) -> Result<String> {
    if program.is_empty() {
        return Err(Error::NotFound);
    }
    if !search || program.contains('/') {
        return Ok(join(cwd, program));
    }
    let mut error = Error::NotFound;
    for directory in path.split(':') {
        let candidate = join(&join(cwd, directory), program);
        match fs::stat(&candidate) {
            Ok(attr) if attr.file_type == fs::FILETYPE_FILE => return Ok(candidate),
            Err(Error::NotAllowed) => error = Error::NotAllowed,
            Err(Error::NotFound | Error::NotADirectory) | Ok(_) => {}
            Err(error) => return Err(error),
        }
    }
    Err(error)
}

fn register(spawned: &process::SpawnResult) -> Result<()> {
    let fd = match fs::open(
        &format!("{}{}", fs::HANDLE_URL_PREFIX, spawned.handle),
        fs::O_HANDLE_CHILD,
    ) {
        Ok(fd) => fd,
        Err(error) => {
            for fd in [spawned.stdin, spawned.stdout, spawned.stderr] {
                if fd >= 0 {
                    let _ = fs::close(fd);
                }
            }
            let killed = process::kill(spawned.handle);
            let waited = process::wait(spawned.handle);
            let released = moto_rt::alloc::release_handle(spawned.handle);
            killed.and(waited).and(released)?;
            return Err(error);
        }
    };
    let child = Arc::new(Child {
        readiness: NativeFd(fd),
        pid: spawned.pid,
        handle: spawned.handle,
        signal: AtomicI32::new(0),
    });
    let mut table = CHILDREN.lock();
    assert!(table.entries.insert(child.pid, child).is_none());
    table.notify();
    Ok(())
}

unsafe fn spawn(options: &SpawnOptions) -> Result<process::SpawnResult> {
    if options.flags & !1 != 0 {
        return Err(Error::InvalidArgument);
    }
    let parent = fs::getcwd()?;
    let cwd = if options.cwd.is_null() {
        parent
    } else {
        let cwd = join(&parent, unsafe { text(options.cwd)? });
        if fs::stat(&cwd)?.file_type != fs::FILETYPE_DIRECTORY {
            return Err(Error::NotADirectory);
        }
        cwd
    };
    let path = if options.path.is_null() {
        "/system/bin:/user/bin:/devtools/bin"
    } else {
        unsafe { text(options.path)? }
    };
    let program = resolve(
        unsafe { text(options.program)? },
        &cwd,
        path,
        options.flags & 1 != 0,
    )?;
    let args = unsafe { strings(options.argv)? }
        .into_iter()
        .skip(1)
        .collect();
    let mut env = Vec::new();
    for pair in unsafe { strings(options.envp)? } {
        if let Some((key, value)) = pair.split_once('=') {
            env.push((key.into(), value.into()));
        }
    }
    let mut stdio = options.stdio;
    for fd in &mut stdio {
        *fd = match *fd {
            0 => process::STDIO_PARENT_STDIN,
            1 => process::STDIO_PARENT_STDOUT,
            2 => process::STDIO_PARENT_STDERR,
            process::STDIO_INHERIT | process::STDIO_NULL | process::STDIO_MAKE_PIPE => *fd,
            fd if fd >= 0 => fd,
            _ => return Err(Error::BadHandle),
        };
    }
    let spawned = process::spawn(process::SpawnArgs {
        program,
        args,
        env,
        cwd: Some(cwd),
        stdin: stdio[0],
        stdout: stdio[1],
        stderr: stdio[2],
    })?;
    register(&spawned)?;
    Ok(spawned)
}

/// Pointer/string contracts and ownership are documented in moto_rt.h.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn moto_rt_spawn_ex(
    options: *const SpawnOptions,
    pid: *mut i32,
    stdio: *mut i32,
) -> i32 {
    if options.is_null()
        || pid.is_null()
        || (stdio.is_null() && unsafe { (*options).stdio }.contains(&process::STDIO_MAKE_PIPE))
    {
        return error(Error::InvalidArgument);
    }
    match unsafe { spawn(&*options) } {
        Ok(spawned) => {
            unsafe {
                *pid = spawned.pid;
                if !stdio.is_null() {
                    core::ptr::copy_nonoverlapping(
                        [spawned.stdin, spawned.stdout, spawned.stderr].as_ptr(),
                        stdio,
                        3,
                    );
                }
            }
            0
        }
        Err(err) => error(err),
    }
}

fn wait(pid: i32, nohang: bool) -> Result<(i32, i32)> {
    if pid != -1 && pid <= 0 {
        return Err(Error::InvalidArgument);
    }
    loop {
        let mut table = CHILDREN.lock();
        let matching: Vec<_> = table
            .entries
            .iter()
            .filter(|(id, _)| pid == -1 || **id == pid)
            .map(|(id, child)| (*id, child.clone()))
            .collect();
        if matching.is_empty() {
            return Err(Error::BadHandle);
        }
        for (id, child) in &matching {
            let mut bytes = [0; 8];
            match fs::read(child.readiness.0, &mut bytes) {
                Ok(8) => {
                    let raw = u64::from_ne_bytes(bytes);
                    let signal = child.signal.load(Ordering::Relaxed);
                    let status = if signal != 0 {
                        signal
                    } else {
                        ((raw as i32) & 255) << 8
                    };
                    table.entries.remove(id);
                    table.notify();
                    return Ok((*id, status));
                }
                Err(Error::NotReady) => {}
                Err(error) => return Err(error),
                Ok(_) => return Err(Error::InternalError),
            }
        }
        if nohang {
            return Ok((0, 0));
        }
        let registry = NativeFd(poll::new()?);
        if table.changed.is_none() {
            table.changed = Some(NativeFd(poll::new()?));
        }
        poll::add(
            registry.0,
            table.changed.as_ref().unwrap().0,
            u64::MAX,
            poll::POLL_READABLE,
        )?;
        for (id, child) in &matching {
            poll::add(
                registry.0,
                child.readiness.0,
                *id as u64,
                poll::POLL_READABLE,
            )?;
        }
        drop(table);
        let mut event = poll::Event::default();
        poll::wait(registry.0, &mut event, 1, None)?;
        // Keep handles alive until the registry no longer borrows them.
        drop(registry);
        drop(matching);
    }
}

/// Returns a reaped pid, zero for WNOHANG, or a negative Motor error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn moto_rt_waitpid_ex(pid: i32, flags: u32, status: *mut i32) -> i32 {
    if flags & !1 != 0 || status.is_null() {
        return error(Error::InvalidArgument);
    }
    match wait(pid, flags & 1 != 0) {
        Ok((pid, value)) => {
            unsafe { *status = value };
            pid
        }
        Err(err) => error(err),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn moto_rt_kill(pid: i32, signal: i32) -> i32 {
    let result = (|| {
        if pid <= 0 {
            return Err(Error::InvalidArgument);
        }
        if signal != 0 && signal != 9 && signal != 15 {
            return Err(Error::NotImplemented);
        }
        let table = CHILDREN.lock();
        if let Some(child) = table.entries.get(&pid) {
            if moto_sys::SysRay::process_status(child.handle.into())
                .map_err(Error::from)?
                .is_some()
            {
                return Err(Error::NotFound);
            }
            if signal != 0 {
                process::kill(child.handle)?;
                child.signal.store(signal, Ordering::Relaxed);
            }
            return Ok(());
        }
        drop(table);
        if signal == 0 {
            let mut info = [moto_sys::stats::ProcessInfoV1::default()];
            let count =
                moto_sys::stats::ProcessInfoV1::list(pid as u64, &mut info).map_err(Error::from)?;
            if count == 1 && info[0].pid == pid as u64 && info[0].active != 0 {
                Ok(())
            } else {
                Err(Error::NotFound)
            }
        } else {
            moto_sys::SysCpu::kill_pid(pid as u64).map_err(Error::from)
        }
    })();
    result.map_or_else(error, |()| 0)
}

static EXECUTABLE_PATH: Mutex<Option<Result<String>>> = Mutex::new(None);

#[unsafe(no_mangle)]
pub extern "C" fn moto_rt_exepath_init() -> i32 {
    let mut cached = EXECUTABLE_PATH.lock();
    match cached.get_or_insert_with(|| {
        let path = process::current_exe()?;
        if path.starts_with('/') {
            Ok(path)
        } else {
            fs::canonicalize(&path)
        }
    }) {
        Ok(_) => 0,
        Err(err) => error(*err),
    }
}

/// Capacity on input, byte count excluding NUL on success; no partial writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn moto_rt_exepath(buffer: *mut u8, size: *mut usize) -> i32 {
    if buffer.is_null() || size.is_null() {
        return error(Error::InvalidArgument);
    }
    let initialized = moto_rt_exepath_init();
    if initialized < 0 {
        return initialized;
    }
    let cached = EXECUTABLE_PATH.lock();
    let bytes = cached.as_ref().unwrap().as_ref().unwrap().as_bytes();
    if bytes.len() >= unsafe { *size } {
        return error(Error::InvalidArgument);
    }
    unsafe {
        core::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, bytes.len());
        buffer.add(bytes.len()).write(0);
        *size = bytes.len();
    }
    0
}
