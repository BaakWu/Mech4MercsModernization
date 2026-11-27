// DirectSound fixes.
//
// Background audio (issue #26). DirectSound mutes every secondary buffer
// whose description lacks DSBCAPS_GLOBALFOCUS while the window that set the
// cooperative level is not the foreground window. In windowed / borderless
// play that silences the game as soon as another window is clicked. GameOS
// loads dsound.dll itself (LoadLibrary + GetProcAddress "DirectSoundCreate");
// the hook loads the same system DLL first, detours its DirectSoundCreate
// export (hot-patch prologue mov edi,edi; push ebp; mov ebp,esp) and, on the
// IDirectSound it returns, patches CreateSoundBuffer (vtable slot 3) to add
// DSBCAPS_GLOBALFOCUS to secondary buffers. Duplicated buffers inherit it.
// Input is unaffected: GameOS still ignores the mouse and keys while the
// window is inactive. [sound] background=0 turns it off.
//
// Lost device (issue #46). On some launches (USB headset as the default
// device) every CreateSoundBuffer on the game's IDirectSound returns
// DSERR_BUFFERLOST from a point early in startup on, for any sound, and
// retrying on that object for 0.5 s never helps: the object itself is left
// broken. GameOS treats any failure as fatal (STOP dialog). When the retries
// fail, the hook:
//   1. creates its own IDirectSound on the same device (same GUID, the
//      game's cooperative level and window) and creates the buffer there;
//      later CreateSoundBuffer / DuplicateSoundBuffer calls on the game's
//      object go to that one. Buffers of separate DirectSound objects mix in
//      Windows' audio engine like any two programs.
//   2. if that fails too, returns a silent buffer: a complete
//      IDirectSoundBuffer (and IDirectSound3DBuffer) that accepts every call,
//      keeps its data in memory and advances its play position in real time,
//      but plays nothing. The game starts without that one sound instead of
//      stopping.
// Only DSERR_BUFFERLOST is handled; other errors reach the game unchanged.
// [sound] recover=0 turns it off. Test aid: [debug] sound_lost=N makes the
// game's object fail from the Nth secondary buffer on (sound_lost_all=1:
// the replacement object too).
#include "hook.h"

#include <mmreg.h>
#include <dsound.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace mw4hook {
namespace {

// Defined here so the hook does not link dxguid.lib / uuid.lib.
const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000,
                            {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIID_IDirectSoundBuffer = {0x279AFA85, 0x4981, 0x11CE,
                                      {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID kIID_IDirectSound3DBuffer = {0x279AFA86, 0x4981, 0x11CE,
                                        {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

void SetVector(D3DVECTOR* v, D3DVALUE x, D3DVALUE y, D3DVALUE z) {
    v->x = x;
    v->y = y;
    v->z = z;
}

using DirectSoundCreate_t = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
using CreateSoundBuffer_t = HRESULT(WINAPI*)(IDirectSound*, LPCDSBUFFERDESC,
                                             LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
using DuplicateSoundBuffer_t = HRESULT(WINAPI*)(IDirectSound*, LPDIRECTSOUNDBUFFER,
                                                LPDIRECTSOUNDBUFFER*);
using SetCooperativeLevel_t = HRESULT(WINAPI*)(IDirectSound*, HWND, DWORD);
using Release_t = ULONG(WINAPI*)(IDirectSound*);

DirectSoundCreate_t g_create = nullptr;  // trampoline
CreateSoundBuffer_t g_create_buffer = nullptr;
DuplicateSoundBuffer_t g_duplicate = nullptr;
SetCooperativeLevel_t g_set_coop = nullptr;
Release_t g_release = nullptr;
volatile LONG g_buffers = 0;
volatile LONG g_requests = 0;  // secondary buffer requests (test aid count)

// Every IDirectSound the game makes (GameOS makes one per device while it
// enumerates them and keeps one; the intro movies' DirectShow renderer makes
// its own on the default device), with what a replacement needs.
struct Device {
    IDirectSound* ds = nullptr;
    GUID guid{};
    bool has_guid = false;
    HWND window = nullptr;
    DWORD level = 0;
    IDirectSound* replacement = nullptr;
    bool replacement_failed = false;
    // The replacement could not be made or lost its buffers too: later
    // requests get one attempt (no retries) before a silent buffer.
    bool silent = false;
};
constexpr int kDevices = 16;
CRITICAL_SECTION g_lock;
Device g_devices[kDevices];  // under g_lock

Device* FindDevice(IDirectSound* ds) {  // lock held; null finds a free entry
    for (Device& d : g_devices) {
        if (d.ds == ds) {
            return &d;
        }
    }
    return nullptr;
}

const char* LevelName(DWORD level) {
    switch (level) {
        case DSSCL_NORMAL: return "normal";
        case DSSCL_PRIORITY: return "priority";
        case DSSCL_EXCLUSIVE: return "exclusive";
        case DSSCL_WRITEPRIMARY: return "writeprimary";
        default: return "?";
    }
}

void FormatGuid(const GUID* guid, char* out, size_t cch) {
    if (!guid) {
        strcpy_s(out, cch, "default");
        return;
    }
    _snprintf_s(out, cch, _TRUNCATE, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                guid->Data1, guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1],
                guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5],
                guid->Data4[6], guid->Data4[7]);
}

void DescribeBuffer(LPCDSBUFFERDESC desc, char* out, size_t cch) {
    const WAVEFORMATEX* f = desc ? desc->lpwfxFormat : nullptr;
    if (!desc) {
        strcpy_s(out, cch, "?");
    } else if (f) {
        _snprintf_s(out, cch, _TRUNCATE, "flags 0x%lX, %u-bit %u ch %lu Hz, %lu bytes",
                    desc->dwFlags, f->wBitsPerSample, f->nChannels, f->nSamplesPerSec,
                    desc->dwBufferBytes);
    } else {
        _snprintf_s(out, cch, _TRUNCATE, "flags 0x%lX, %lu bytes", desc->dwFlags,
                    desc->dwBufferBytes);
    }
}

// ---------------------------------------------------------------------------
// Silent buffer

class SilentBuffer;

class Silent3D final : public IDirectSound3DBuffer {
public:
    explicit Silent3D(SilentBuffer* owner) : owner_(owner) {
        params_.dwSize = sizeof(params_);
        params_.vConeOrientation.z = 1.0f;
        params_.dwInsideConeAngle = DS3D_DEFAULTCONEANGLE;
        params_.dwOutsideConeAngle = DS3D_DEFAULTCONEANGLE;
        params_.lConeOutsideVolume = DS3D_DEFAULTCONEOUTSIDEVOLUME;
        params_.flMinDistance = DS3D_DEFAULTMINDISTANCE;
        params_.flMaxDistance = DS3D_DEFAULTMAXDISTANCE;
        params_.dwMode = DS3DMODE_NORMAL;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetAllParameters(LPDS3DBUFFER p) override {
        if (!p || p->dwSize < sizeof(DS3DBUFFER)) return DSERR_INVALIDPARAM;
        *p = params_;
        return DS_OK;
    }
    STDMETHODIMP GetConeAngles(LPDWORD inside, LPDWORD outside) override {
        if (inside) *inside = params_.dwInsideConeAngle;
        if (outside) *outside = params_.dwOutsideConeAngle;
        return DS_OK;
    }
    STDMETHODIMP GetConeOrientation(D3DVECTOR* v) override {
        if (v) *v = params_.vConeOrientation;
        return DS_OK;
    }
    STDMETHODIMP GetConeOutsideVolume(LPLONG v) override {
        if (v) *v = params_.lConeOutsideVolume;
        return DS_OK;
    }
    STDMETHODIMP GetMaxDistance(D3DVALUE* v) override {
        if (v) *v = params_.flMaxDistance;
        return DS_OK;
    }
    STDMETHODIMP GetMinDistance(D3DVALUE* v) override {
        if (v) *v = params_.flMinDistance;
        return DS_OK;
    }
    STDMETHODIMP GetMode(LPDWORD v) override {
        if (v) *v = params_.dwMode;
        return DS_OK;
    }
    STDMETHODIMP GetPosition(D3DVECTOR* v) override {
        if (v) *v = params_.vPosition;
        return DS_OK;
    }
    STDMETHODIMP GetVelocity(D3DVECTOR* v) override {
        if (v) *v = params_.vVelocity;
        return DS_OK;
    }
    STDMETHODIMP SetAllParameters(LPCDS3DBUFFER p, DWORD) override {
        if (!p || p->dwSize < sizeof(DS3DBUFFER)) return DSERR_INVALIDPARAM;
        params_ = *p;
        return DS_OK;
    }
    STDMETHODIMP SetConeAngles(DWORD inside, DWORD outside, DWORD) override {
        params_.dwInsideConeAngle = inside;
        params_.dwOutsideConeAngle = outside;
        return DS_OK;
    }
    STDMETHODIMP SetConeOrientation(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD) override {
        SetVector(&params_.vConeOrientation, x, y, z);
        return DS_OK;
    }
    STDMETHODIMP SetConeOutsideVolume(LONG v, DWORD) override {
        params_.lConeOutsideVolume = v;
        return DS_OK;
    }
    STDMETHODIMP SetMaxDistance(D3DVALUE v, DWORD) override {
        params_.flMaxDistance = v;
        return DS_OK;
    }
    STDMETHODIMP SetMinDistance(D3DVALUE v, DWORD) override {
        params_.flMinDistance = v;
        return DS_OK;
    }
    STDMETHODIMP SetMode(DWORD v, DWORD) override {
        params_.dwMode = v;
        return DS_OK;
    }
    STDMETHODIMP SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD) override {
        SetVector(&params_.vPosition, x, y, z);
        return DS_OK;
    }
    STDMETHODIMP SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD) override {
        SetVector(&params_.vVelocity, x, y, z);
        return DS_OK;
    }

private:
    SilentBuffer* owner_;
    DS3DBUFFER params_{};
};

class SilentBuffer final : public IDirectSoundBuffer {
public:
    static HRESULT Create(DWORD flags, DWORD bytes, const WAVEFORMATEX* format,
                          LPDIRECTSOUNDBUFFER* out) {
        if (!out) {
            return DSERR_INVALIDPARAM;
        }
        *out = nullptr;
        if (bytes < 4) {
            bytes = 4;
        }
        if (bytes > 64 * 1024 * 1024) {
            return DSERR_OUTOFMEMORY;
        }
        auto* data = static_cast<BYTE*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes));
        if (!data) {
            return DSERR_OUTOFMEMORY;
        }
        void* memory = HeapAlloc(GetProcessHeap(), 0, sizeof(SilentBuffer));
        if (!memory) {
            HeapFree(GetProcessHeap(), 0, data);
            return DSERR_OUTOFMEMORY;
        }
        auto* buffer = new (memory) SilentBuffer(flags, bytes, format, data);
        vtable_ = *reinterpret_cast<void**>(buffer);
        *out = buffer;
        return DS_OK;
    }

    // True for a buffer made here (none exist before the first Create).
    static bool Is(void* object) {
        if (!vtable_) {
            return false;
        }
        __try {
            return *static_cast<void**>(object) == vtable_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    HRESULT Duplicate(LPDIRECTSOUNDBUFFER* out) {
        return Create(flags_, bytes_, &format_.Format, out);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (IsEqualGUID(iid, kIID_IUnknown) || IsEqualGUID(iid, kIID_IDirectSoundBuffer)) {
            *out = static_cast<IDirectSoundBuffer*>(this);
        } else if (IsEqualGUID(iid, kIID_IDirectSound3DBuffer)) {
            *out = &three_d_;
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) {
            BYTE* data = data_;
            this->~SilentBuffer();
            HeapFree(GetProcessHeap(), 0, data);
            HeapFree(GetProcessHeap(), 0, this);
        }
        return static_cast<ULONG>(refs);
    }

    // IDirectSoundBuffer
    STDMETHODIMP GetCaps(LPDSBCAPS caps) override {
        if (!caps || caps->dwSize < sizeof(DSBCAPS)) return DSERR_INVALIDPARAM;
        caps->dwFlags = (flags_ & ~(DSBCAPS_LOCHARDWARE | DSBCAPS_LOCDEFER)) |
                        DSBCAPS_LOCSOFTWARE;
        caps->dwBufferBytes = bytes_;
        caps->dwUnlockTransferRate = 0;
        caps->dwPlayCpuOverhead = 0;
        return DS_OK;
    }
    STDMETHODIMP GetCurrentPosition(LPDWORD play, LPDWORD write) override {
        AcquireSRWLockExclusive(&lock_);
        const DWORD pos = Advance();
        ReleaseSRWLockExclusive(&lock_);
        if (play) *play = pos;
        if (write) *write = pos;
        return DS_OK;
    }
    STDMETHODIMP GetFormat(LPWAVEFORMATEX format, DWORD size, LPDWORD written) override {
        const DWORD need = sizeof(WAVEFORMATEX) + format_.Format.cbSize;
        if (written) *written = need;
        if (!format) return written ? DS_OK : DSERR_INVALIDPARAM;
        const DWORD copy = size < need ? size : need;
        memcpy(format, &format_, copy);
        return DS_OK;
    }
    STDMETHODIMP GetVolume(LPLONG v) override {
        if (!v) return DSERR_INVALIDPARAM;
        *v = volume_;
        return DS_OK;
    }
    STDMETHODIMP GetPan(LPLONG v) override {
        if (!v) return DSERR_INVALIDPARAM;
        *v = pan_;
        return DS_OK;
    }
    STDMETHODIMP GetFrequency(LPDWORD v) override {
        if (!v) return DSERR_INVALIDPARAM;
        *v = frequency_ ? frequency_ : format_.Format.nSamplesPerSec;
        return DS_OK;
    }
    STDMETHODIMP GetStatus(LPDWORD status) override {
        if (!status) return DSERR_INVALIDPARAM;
        AcquireSRWLockExclusive(&lock_);
        Advance();
        *status = playing_ ? (DSBSTATUS_PLAYING | (looping_ ? DSBSTATUS_LOOPING : 0)) : 0;
        ReleaseSRWLockExclusive(&lock_);
        return DS_OK;
    }
    STDMETHODIMP Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override {
        return DSERR_ALREADYINITIALIZED;
    }
    STDMETHODIMP Lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD b1, LPVOID* p2,
                      LPDWORD b2, DWORD flags) override {
        if (!p1 || !b1) return DSERR_INVALIDPARAM;
        if (flags & DSBLOCK_FROMWRITECURSOR) {
            AcquireSRWLockExclusive(&lock_);
            offset = Advance();
            ReleaseSRWLockExclusive(&lock_);
        }
        if (flags & DSBLOCK_ENTIREBUFFER) {
            bytes = bytes_;
        }
        if (offset >= bytes_ || bytes > bytes_ || bytes == 0) return DSERR_INVALIDPARAM;
        const DWORD first = (bytes < bytes_ - offset) ? bytes : bytes_ - offset;
        *p1 = data_ + offset;
        *b1 = first;
        if (p2) *p2 = (bytes > first) ? data_ : nullptr;
        if (b2) *b2 = bytes - first;
        return DS_OK;
    }
    STDMETHODIMP Play(DWORD, DWORD, DWORD flags) override {
        AcquireSRWLockExclusive(&lock_);
        Advance();
        looping_ = (flags & DSBPLAY_LOOPING) != 0;
        if (!playing_) {
            playing_ = true;
            started_ = Now();
            start_pos_ = position_;
        }
        ReleaseSRWLockExclusive(&lock_);
        return DS_OK;
    }
    STDMETHODIMP SetCurrentPosition(DWORD pos) override {
        AcquireSRWLockExclusive(&lock_);
        position_ = start_pos_ = (bytes_ ? pos % bytes_ : 0);
        started_ = Now();
        ReleaseSRWLockExclusive(&lock_);
        return DS_OK;
    }
    STDMETHODIMP SetFormat(LPCWAVEFORMATEX) override { return DSERR_INVALIDCALL; }
    STDMETHODIMP SetVolume(LONG v) override {
        volume_ = v;
        return DS_OK;
    }
    STDMETHODIMP SetPan(LONG v) override {
        pan_ = v;
        return DS_OK;
    }
    STDMETHODIMP SetFrequency(DWORD v) override {
        AcquireSRWLockExclusive(&lock_);
        Advance();
        frequency_ = v;
        started_ = Now();
        start_pos_ = position_;
        ReleaseSRWLockExclusive(&lock_);
        return DS_OK;
    }
    STDMETHODIMP Stop() override {
        AcquireSRWLockExclusive(&lock_);
        Advance();
        playing_ = false;
        ReleaseSRWLockExclusive(&lock_);
        return DS_OK;
    }
    STDMETHODIMP Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }
    STDMETHODIMP Restore() override { return DS_OK; }

private:
    friend class Silent3D;

    SilentBuffer(DWORD flags, DWORD bytes, const WAVEFORMATEX* format, BYTE* data)
        : flags_(flags), bytes_(bytes), data_(data), three_d_(this) {
        if (format) {
            // PCM has no extension; anything longer than WAVEFORMATEXTENSIBLE's
            // is cut (GetFormat then reports the stored size).
            constexpr DWORD kMaxExtra = sizeof(format_) - sizeof(WAVEFORMATEX);
            DWORD extra = format->wFormatTag == WAVE_FORMAT_PCM ? 0 : format->cbSize;
            if (extra > kMaxExtra) {
                extra = kMaxExtra;
            }
            memcpy(&format_, format, sizeof(WAVEFORMATEX) + extra);
            format_.Format.cbSize = static_cast<WORD>(extra);
        } else {
            format_.Format.wFormatTag = WAVE_FORMAT_PCM;
            format_.Format.nChannels = 1;
            format_.Format.nSamplesPerSec = 22050;
            format_.Format.wBitsPerSample = 16;
            format_.Format.nBlockAlign = 2;
            format_.Format.nAvgBytesPerSec = 44100;
        }
    }
    ~SilentBuffer() = default;

    static ULONGLONG Now() { return GetTickCount64(); }

    // Moves the play position on by the time since the last update (lock held).
    DWORD Advance() {
        if (!playing_ || bytes_ == 0) {
            return position_;
        }
        const WAVEFORMATEX& f = format_.Format;
        const double rate = (frequency_ && f.nSamplesPerSec)
                                ? f.nAvgBytesPerSec * (static_cast<double>(frequency_) /
                                                       f.nSamplesPerSec)
                                : f.nAvgBytesPerSec;
        const double moved = rate * static_cast<double>(Now() - started_) / 1000.0;
        double pos = start_pos_ + moved;
        if (pos >= bytes_) {
            if (looping_) {
                pos = static_cast<double>(static_cast<ULONGLONG>(pos) % bytes_);
            } else {
                playing_ = false;
                position_ = 0;
                return position_;
            }
        }
        DWORD p = static_cast<DWORD>(pos);
        if (f.nBlockAlign > 1) {
            p -= p % f.nBlockAlign;
        }
        position_ = p;
        return position_;
    }

    static void* vtable_;

    volatile LONG refs_ = 1;
    DWORD flags_;
    DWORD bytes_;
    BYTE* data_;
    WAVEFORMATEXTENSIBLE format_{};
    LONG volume_ = 0;
    LONG pan_ = 0;
    DWORD frequency_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
    bool playing_ = false;
    bool looping_ = false;
    DWORD position_ = 0;
    DWORD start_pos_ = 0;
    ULONGLONG started_ = 0;
    Silent3D three_d_;
};

void* SilentBuffer::vtable_ = nullptr;

HRESULT Silent3D::QueryInterface(REFIID iid, void** out) {
    return owner_->QueryInterface(iid, out);
}
ULONG Silent3D::AddRef() { return owner_->AddRef(); }
ULONG Silent3D::Release() { return owner_->Release(); }

volatile LONG g_silent = 0;

HRESULT MakeSilent(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out) {
    const HRESULT result = SilentBuffer::Create(desc->dwFlags, desc->dwBufferBytes,
                                                desc->lpwfxFormat, out);
    const LONG n = InterlockedIncrement(&g_silent);
    if (n <= 3 || n % 100 == 0) {
        char text[128];
        DescribeBuffer(desc, text, sizeof(text));
        Log("sound: %s -> silent buffer %ld (%08lX); the game goes on without this sound\n",
            text, n, static_cast<unsigned long>(result));
    }
    return result;
}

// A silent copy of a real buffer that could not be duplicated.
HRESULT MakeSilentCopy(LPDIRECTSOUNDBUFFER source, LPDIRECTSOUNDBUFFER* out) {
    DSBCAPS caps{};
    caps.dwSize = sizeof(caps);
    union {
        WAVEFORMATEX format;
        unsigned char raw[64];
    } fmt{};
    DSBUFFERDESC desc{};
    desc.dwSize = sizeof(desc);
    desc.dwBufferBytes = 0x10000;
    __try {
        if (SUCCEEDED(source->GetCaps(&caps))) {
            desc.dwFlags = caps.dwFlags;
            desc.dwBufferBytes = caps.dwBufferBytes;
        }
        DWORD written = 0;
        if (SUCCEEDED(source->GetFormat(&fmt.format, sizeof(fmt), &written))) {
            desc.lpwfxFormat = &fmt.format;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return MakeSilent(&desc, out);
}

// ---------------------------------------------------------------------------
// IDirectSound hooks

bool SimulateLost(IDirectSound* target) {
    const Config& config = GetConfig();
    if (config.debug_sound_lost <= 0 || g_requests < config.debug_sound_lost) {
        return false;
    }
    // GameOS opens its device by GUID; the movies' DirectShow renderer uses
    // the default device and retries a failed buffer forever (the movie
    // stalls), so the test aid leaves it alone.
    bool lost = false;
    EnterCriticalSection(&g_lock);
    for (const Device& d : g_devices) {
        if (d.ds && d.has_guid &&
            (d.ds == target || (config.debug_sound_lost_all && d.replacement == target))) {
            lost = true;
        }
    }
    LeaveCriticalSection(&g_lock);
    return lost;
}

HRESULT CallCreate(IDirectSound* target, LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* buffer,
                   LPUNKNOWN outer) {
    if (!(desc->dwFlags & DSBCAPS_PRIMARYBUFFER) && SimulateLost(target)) {
        *buffer = nullptr;
        return DSERR_BUFFERLOST;
    }
    return g_create_buffer(target, desc, buffer, outer);
}

// CreateSoundBuffer can fail with DSERR_BUFFERLOST while Windows briefly
// reports the audio endpoint as unavailable. A lost-buffer failure is retried
// for up to ~250 ms before anything else is tried.
HRESULT CreateRetrying(IDirectSound* target, LPCDSBUFFERDESC desc,
                       LPDIRECTSOUNDBUFFER* buffer, LPUNKNOWN outer, int tries = 25) {
    HRESULT result = CallCreate(target, desc, buffer, outer);
    int retries = 0;
    for (; result == DSERR_BUFFERLOST && retries < tries; ++retries) {
        Sleep(10);
        result = CallCreate(target, desc, buffer, outer);
    }
    if (retries > 0) {
        Log("sound: CreateSoundBuffer lost the buffer; %d retries -> %08lX\n", retries,
            static_cast<unsigned long>(result));
    }
    return result;
}

// A new IDirectSound on the device of |d|, set up like it (lock held).
IDirectSound* Replacement(Device* d) {
    if (d->replacement || d->replacement_failed || !g_create || !g_set_coop) {
        return d->replacement;
    }
    char device[48];
    FormatGuid(d->has_guid ? &d->guid : nullptr, device, sizeof(device));
    IDirectSound* ds = nullptr;
    DWORD level = d->level ? d->level : DSSCL_PRIORITY;
    // The trampoline: the new object is not tracked as one of the game's.
    HRESULT result = g_create(d->has_guid ? &d->guid : nullptr, &ds, nullptr);
    if (SUCCEEDED(result) && ds) {
        HWND window = d->window;
        if (!window || !IsWindow(window)) {
            window = MainWindow();
        }
        if (!window) {
            window = GetDesktopWindow();
        }
        result = g_set_coop(ds, window, level);
        if (FAILED(result) && level != DSSCL_NORMAL) {
            result = g_set_coop(ds, window, DSSCL_NORMAL);
        }
        if (FAILED(result)) {
            g_release(ds);
            ds = nullptr;
        }
    }
    if (!ds) {
        d->replacement_failed = true;
        Log("sound: new DirectSound on device %s FAILED (%08lX)\n", device,
            static_cast<unsigned long>(result));
        return nullptr;
    }
    d->replacement = ds;
    Log("sound: DirectSound %p keeps losing buffers; new DirectSound %p on device %s "
        "(%s level) takes its buffers from now on\n",
        static_cast<void*>(d->ds), static_cast<void*>(ds), device, LevelName(level));
    return ds;
}

// The object a call on |self| goes to: its replacement once there is one.
IDirectSound* Routed(IDirectSound* self, bool* silent) {
    EnterCriticalSection(&g_lock);
    Device* d = FindDevice(self);
    IDirectSound* target = (d && d->replacement) ? d->replacement : self;
    *silent = d && d->silent;
    LeaveCriticalSection(&g_lock);
    return target;
}

void MarkSilent(IDirectSound* self) {
    EnterCriticalSection(&g_lock);
    Device* d = FindDevice(self);
    const bool first = d && !d->silent;
    if (d) {
        d->silent = true;
    }
    LeaveCriticalSection(&g_lock);
    if (first) {
        Log("sound: DirectSound %p: buffers that still fail are silent from now on (no retries)\n",
            static_cast<void*>(self));
    }
}

HRESULT WINAPI Hook_CreateSoundBuffer(IDirectSound* self, LPCDSBUFFERDESC desc,
                                      LPDIRECTSOUNDBUFFER* buffer, LPUNKNOWN outer) {
    if (!desc || !buffer || desc->dwSize < sizeof(DSBUFFERDESC1)) {
        return g_create_buffer(self, desc, buffer, outer);
    }
    const bool primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (!primary) {
        InterlockedIncrement(&g_requests);
    }
    const Config& config = GetConfig();
    bool silent = false;
    IDirectSound* target = primary ? self : Routed(self, &silent);
    const int tries = (silent && config.sound_recover) ? 0 : 25;

    DSBUFFERDESC copy{};
    LPCDSBUFFERDESC use = desc;
    if (config.sound_background && !primary && !(desc->dwFlags & DSBCAPS_GLOBALFOCUS)) {
        memcpy(&copy, desc, desc->dwSize < sizeof(copy) ? desc->dwSize : sizeof(copy));
        copy.dwFlags |= DSBCAPS_GLOBALFOCUS;
        use = &copy;
    }
    HRESULT result = CreateRetrying(target, use, buffer, outer, tries);
    if (FAILED(result) && result != DSERR_BUFFERLOST && use != desc) {
        // Not expected; fall back to the game's own description.
        Log("sound: CreateSoundBuffer with GLOBALFOCUS failed (%08lX); unchanged\n",
            static_cast<unsigned long>(result));
        use = desc;
        result = CreateRetrying(target, use, buffer, outer, tries);
    }
    if (result == DSERR_BUFFERLOST && !primary && config.sound_recover && silent) {
        return MakeSilent(desc, buffer);
    }
    if (result == DSERR_BUFFERLOST && !primary && config.sound_recover) {
        char text[128];
        DescribeBuffer(desc, text, sizeof(text));
        Log("sound: CreateSoundBuffer (%s) on %p still DSERR_BUFFERLOST\n", text,
            static_cast<void*>(target));
        IDirectSound* replacement = nullptr;
        if (target == self) {
            EnterCriticalSection(&g_lock);
            if (Device* d = FindDevice(self)) {
                replacement = Replacement(d);
            }
            LeaveCriticalSection(&g_lock);
        }
        if (replacement) {
            result = CreateRetrying(replacement, use, buffer, outer);
            Log("sound: buffer on the new DirectSound -> %08lX\n",
                static_cast<unsigned long>(result));
        }
        if (result == DSERR_BUFFERLOST) {
            MarkSilent(self);
            return MakeSilent(desc, buffer);
        }
    }
    if (SUCCEEDED(result) && use == &copy && InterlockedIncrement(&g_buffers) == 1) {
        Log("sound: buffers keep playing in the background (DSBCAPS_GLOBALFOCUS)\n");
    }
    return result;
}

HRESULT CallDuplicate(IDirectSound* target, LPDIRECTSOUNDBUFFER source,
                      LPDIRECTSOUNDBUFFER* out) {
    if (SimulateLost(target)) {
        *out = nullptr;
        return DSERR_BUFFERLOST;
    }
    return g_duplicate(target, source, out);
}

HRESULT DuplicateOn(IDirectSound* self, IDirectSound* target, LPDIRECTSOUNDBUFFER source,
                    LPDIRECTSOUNDBUFFER* out) {
    HRESULT result = CallDuplicate(target, source, out);
    if (FAILED(result) && target != self) {
        // The source belongs to the game's own object (made before the switch).
        result = CallDuplicate(self, source, out);
    }
    return result;
}

HRESULT WINAPI Hook_DuplicateSoundBuffer(IDirectSound* self, LPDIRECTSOUNDBUFFER source,
                                         LPDIRECTSOUNDBUFFER* out) {
    if (!source || !out) {
        return g_duplicate(self, source, out);
    }
    if (SilentBuffer::Is(source)) {
        return static_cast<SilentBuffer*>(source)->Duplicate(out);
    }
    InterlockedIncrement(&g_requests);
    bool silent = false;
    IDirectSound* target = Routed(self, &silent);
    const int tries = (silent && GetConfig().sound_recover) ? 0 : 25;
    HRESULT result = DuplicateOn(self, target, source, out);
    int retries = 0;
    for (; result == DSERR_BUFFERLOST && retries < tries; ++retries) {
        Sleep(10);
        result = DuplicateOn(self, target, source, out);
    }
    if (retries > 0) {
        Log("sound: DuplicateSoundBuffer lost the buffer; %d retries -> %08lX\n", retries,
            static_cast<unsigned long>(result));
    }
    if (result == DSERR_BUFFERLOST && GetConfig().sound_recover) {
        return MakeSilentCopy(source, out);
    }
    return result;
}

HRESULT WINAPI Hook_SetCooperativeLevel(IDirectSound* self, HWND window, DWORD level) {
    const HRESULT result = g_set_coop(self, window, level);
    EnterCriticalSection(&g_lock);
    Device* d = FindDevice(self);
    if (d) {
        d->window = window;
        d->level = level;
        if (d->replacement) {
            g_set_coop(d->replacement, window, level);
        }
    }
    LeaveCriticalSection(&g_lock);
    if (d) {
        Log("sound: SetCooperativeLevel(%p, window %p, %s) -> %08lX\n",
            static_cast<void*>(self), static_cast<void*>(window), LevelName(level),
            static_cast<unsigned long>(result));
    }
    return result;
}

ULONG WINAPI Hook_Release(IDirectSound* self) {
    const ULONG refs = g_release(self);
    if (refs != 0) {
        return refs;
    }
    EnterCriticalSection(&g_lock);
    Device* d = FindDevice(self);
    IDirectSound* replacement = nullptr;
    if (d) {
        replacement = d->replacement;
        *d = Device{};
    }
    LeaveCriticalSection(&g_lock);
    if (replacement) {
        g_release(replacement);
    }
    if (d) {
        Log("sound: DirectSound %p released%s\n", static_cast<void*>(self),
            replacement ? " (and its replacement)" : "");
    }
    return refs;
}

bool PatchSlot(void** vtable, int slot, void* hook, void** original) {
    if (vtable[slot] == hook) {
        return true;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    if (!*original) {
        *original = vtable[slot];
    }
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return true;
}

void PatchDirectSound(IDirectSound* ds) {
    void** vtable = *reinterpret_cast<void***>(ds);
    PatchSlot(vtable, 3, reinterpret_cast<void*>(&Hook_CreateSoundBuffer),
              reinterpret_cast<void**>(&g_create_buffer));
    PatchSlot(vtable, 2, reinterpret_cast<void*>(&Hook_Release),
              reinterpret_cast<void**>(&g_release));
    PatchSlot(vtable, 5, reinterpret_cast<void*>(&Hook_DuplicateSoundBuffer),
              reinterpret_cast<void**>(&g_duplicate));
    PatchSlot(vtable, 6, reinterpret_cast<void*>(&Hook_SetCooperativeLevel),
              reinterpret_cast<void**>(&g_set_coop));
}

HRESULT WINAPI Hook_DirectSoundCreate(LPCGUID device, LPDIRECTSOUND* ds, LPUNKNOWN outer) {
    const HRESULT result = g_create(device, ds, outer);
    char name[48];
    FormatGuid(device, name, sizeof(name));
    Log("sound: DirectSoundCreate(%s) -> %08lX %p\n", name, static_cast<unsigned long>(result),
        (ds && SUCCEEDED(result)) ? static_cast<void*>(*ds) : nullptr);
    if (SUCCEEDED(result) && ds && *ds) {
        PatchDirectSound(*ds);
        EnterCriticalSection(&g_lock);
        Device* d = FindDevice(*ds);  // an address reused after a Release
        if (!d) {
            d = FindDevice(nullptr);
        }
        if (d) {
            *d = Device{};
            d->ds = *ds;
            d->has_guid = device != nullptr;
            if (device) {
                d->guid = *device;
            }
        }
        LeaveCriticalSection(&g_lock);
        if (!d) {
            Log("sound: more than %d DirectSound objects; %p not tracked\n", kDevices,
                static_cast<void*>(*ds));
        }
    }
    return result;
}

bool g_installed = false;

}  // namespace

bool InstallSoundHooks() {
    if (g_installed) {
        return true;
    }
    g_installed = true;
    InitializeCriticalSection(&g_lock);
    const Config& config = GetConfig();
    if (!config.sound_background && !config.sound_recover) {
        return true;
    }
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (!len || len > MAX_PATH - 16) {
        return true;
    }
    wcscat_s(path, L"\\dsound.dll");
    HMODULE dsound = LoadLibraryW(path);
    auto* target = dsound ? reinterpret_cast<unsigned char*>(
                                GetProcAddress(dsound, "DirectSoundCreate"))
                          : nullptr;
    // mov edi,edi; push ebp; mov ebp,esp: 5 bytes, position independent.
    static const unsigned char kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!target || memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
        Log("sound: DirectSoundCreate not hookable; DirectSound unchanged\n");
        return true;
    }
    InstallDetour(target, 5, reinterpret_cast<void*>(&Hook_DirectSoundCreate),
                  reinterpret_cast<void**>(&g_create));
    Log("sound: DirectSoundCreate hook %s (background %d, recover %d%s)\n",
        g_create ? "ok" : "FAILED", config.sound_background ? 1 : 0,
        config.sound_recover ? 1 : 0,
        config.debug_sound_lost > 0 ? ", TEST: simulated lost buffers" : "");
    return true;
}

}  // namespace mw4hook
