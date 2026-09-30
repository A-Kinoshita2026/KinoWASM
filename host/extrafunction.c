/* #define _CRT_SECURE_NO_WARNINGS */

/* rand_s (RtlGenRandom ラッパ) を有効化。<stdlib.h> より前で定義する必要がある。 */
#define _CRT_RAND_S

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "winapi.h"
#include "kw_store.h"
#include "kinowasm.h"

#define WASI_MODULE "wasi_snapshot_preview1"

#define GETPARAM_INT(index) api_call->args->data[index].val.num.i32
#define GETPARAM_FLOAT(index) api_call->args->data[index].val.num.f32
#define SETRET_INT(index, value) do { \
	api_call->rets->data[index].type = TYPE_VAL_I32; \
	api_call->rets->data[index].val.num.i32 = value; \
} while(0)
#define SETRET_FLOAT(index, value) do { \
	api_call->rets->data[index].type = TYPE_VAL_F32; \
	api_call->rets->data[index].val.num.f32 = value; \
} while(0)
#define __WASI_CLOCK_REALTIME           (0)
#define __WASI_CLOCK_MONOTONIC          (1)
#define __WASI_CLOCK_PROCESS_CPUTIME_ID (2)
#define __WASI_CLOCK_THREAD_CPUTIME_ID  (3)

/* WASI errno 値 (preview1 spec 準拠)。
 * 注: 既存の __WASI_ESPIPE (29) は spec 上は EIO の値と一致 (spec の正規 ESPIPE は 70)。
 *     互換性のため既存定義は据え置き、内部用に __WASI_EIO を別名で追加する。 */
#define __WASI_ESUCCESS  (0)
#define __WASI_EACCES    (2)
#define __WASI_EBADF     (8)
#define __WASI_EEXIST    (20)
#define __WASI_EFAULT    (21)
#define __WASI_EINVAL    (28)
#define __WASI_EIO       (29)
#define __WASI_EISDIR    (31)
#define __WASI_ELOOP     (32)
#define __WASI_ENOENT    (44)
#define __WASI_ENOMEM    (48)
#define __WASI_ENOSYS    (52)
#define __WASI_ENOTDIR   (54)
#define __WASI_ENOTEMPTY (55)
#define __WASI_EOVERFLOW (61)
#define __WASI_EPERM     (63)
#define __WASI_ESPIPE    (29)

/* host の errno を WASI errno に変換する。失敗パスで使う。 */
static uint32_t wasi_errno_from_host(int e)
{
	switch(e) {
		case 0:        return __WASI_ESUCCESS;
		case ENOENT:   return __WASI_ENOENT;
		case EEXIST:   return __WASI_EEXIST;
		case EACCES:   return __WASI_EACCES;
		case EISDIR:   return __WASI_EISDIR;
#ifdef ENOTDIR
		case ENOTDIR:  return __WASI_ENOTDIR;
#endif
#ifdef ENOTEMPTY
		case ENOTEMPTY:return __WASI_ENOTEMPTY;
#endif
#ifdef ELOOP
		case ELOOP:    return __WASI_ELOOP;
#endif
#ifdef EPERM
		case EPERM:    return __WASI_EPERM;
#endif
		case EINVAL:   return __WASI_EINVAL;
		default:       return __WASI_EIO;
	}
}

/* WASI clockid (preview1 spec)
 * __WASI_CLOCK_REALTIME / MONOTONIC / PROCESS_CPUTIME_ID / THREAD_CPUTIME_ID は上で定義済み */

#define __WASI_FILETYPE_UNKNOWN (0)
#define __WASI_FILETYPE_BLOCK_DEVICE (1)
#define __WASI_FILETYPE_CHARACTER_DEVICE (2)
#define __WASI_FILETYPE_DIRECTORY (3)
#define __WASI_FILETYPE_REGULAR_FILE (4)
#define __WASI_FILETYPE_SOCKET_DGRAM (5)
#define __WASI_FILETYPE_SOCKET_STREAM (6)
#define __WASI_FILETYPE_SYMBOLIC_LINK (7)

#define __WASI_RIGHT_FD_READ ((uint64_t)1u << 1)
#define __WASI_RIGHT_FD_SEEK ((uint64_t)1u << 2)
#define __WASI_RIGHT_FD_FDSTAT_SET_FLAGS ((uint64_t)1u << 3)
#define __WASI_RIGHT_FD_TELL ((uint64_t)1u << 5)
#define __WASI_RIGHT_FD_WRITE ((uint64_t)1u << 6)
#define __WASI_RIGHT_FD_FILESTAT_GET ((uint64_t)1u << 21)
#define __WASI_RIGHT_PATH_OPEN ((uint64_t)1u << 13)
#define __WASI_RIGHT_PATH_FILESTAT_GET ((uint64_t)1u << 18)
#define __WASI_RIGHT_POLL_FD_READWRITE ((uint64_t)1u << 27)

/* preopen tag (variant 内の discriminator) */
#define __WASI_PREOPENTYPE_DIR (0)

/* path_open oflags (現在は O_DIRECTORY のみ尊重、それ以外は無視) */
#define __WASI_OFLAGS_CREAT     (1 << 0)
#define __WASI_OFLAGS_DIRECTORY (1 << 1)
#define __WASI_OFLAGS_EXCL      (1 << 2)
#define __WASI_OFLAGS_TRUNC     (1 << 3)

/* fd_seek whence */
#define __WASI_WHENCE_SET (0)
#define __WASI_WHENCE_CUR (1)
#define __WASI_WHENCE_END (2)

#define COMPARE_STRING(a , b) (strlen((const char*)a) == strlen((const char*)b) && memcmp(a, b, strlen(b)) == 0)

typedef uint8_t __wasi_filetype_t;
typedef uint16_t __wasi_fdflags_t;
typedef uint64_t __wasi_rights_t;

typedef struct {
	__wasi_filetype_t fs_filetype;
	__wasi_fdflags_t fs_flags;
	__wasi_rights_t fs_rights_base;
	__wasi_rights_t fs_rights_inheriting;
} __wasi_fdstat_t;

typedef enum {
	ERR_SEGMENTATION_FAULT = 201,
	ERR_ALIGNMENT_FAULT = 202,
	INVALID_ARGUMENT = 203
} error_code_t;

typedef struct memorypool {
	struct memorypool* next;
	struct memorypool* prev;
	size_t memory_size;
	uint32_t memory_addr;
} memorypool_t;

typedef struct {
	kinowasm_handle_t S;
	memorypool_t unuse_memory;
	memorypool_t use_memory;
} memorytable_t;

typedef struct {
	size_t funcs_len;
	size_t tables_len;
	size_t memorys_len;
	size_t globals_len;
	size_t elements_len;
	size_t datas_len;
	size_t moduletable_len;
	void* module_memory;
} modulepushinfo_t;

typedef struct {
	uint8_t is_used;
	uint8_t is_dirty;
	uint32_t module_memory_size;
	uint32_t script_memory_size;
	kinowasm_handle_t S;
	void* script_memory;
	modulepushinfo_t* push_infos;
	size_t push_info_len;
	size_t push_info_capacity;
} modulestore_t;

typedef kinowasm_callinfo_t* api_call_t;

extern kinowasm_settings_t kinowasm_settings;
kinowasm_result_t validate_function_parameter(kinowasm_handle_t handle);

static kinowasm_array(memorytable_t) memory_table = { NULL, 0, 0, 0 };
static uint32_t memory_count = 1;
static modulestore_t* module_stores = NULL;
static size_t module_store_len = 0;
static size_t module_store_capacity = 0;

/* WASI argv / environ をホスト (KinoRuntime.exe / testsuite_runner 等) から
 * 受け取って WASM 側に渡すためのグローバル state。NULL 状態は「無し」を意味する。
 * 値は呼び出し側が所有する文字列 (ホストの argv / _environ 等) を保持するだけで
 * コピーは行わない。プロセスが生きている間はポインタが有効である前提。 */
static int wasi_argc = 0;
static const char* const* wasi_argv = NULL;
static const char* const* wasi_envp = NULL;

/* proc_exit が呼ばれたかどうかと、その時点での exit code。
 * proc_exit 自体は trap (戻り値非 0) を返すため、ホストは
 * kinowasm_invoke のエラー後に wasi_get_exit_status() で
 * 「正常な exit か trap か」を判別する。 */
static int     wasi_exit_called = 0;
static int32_t wasi_exit_code   = 0;

void wasi_set_args(int argc, const char* const* argv)
{
	wasi_argc = (argc > 0) ? argc : 0;
	wasi_argv = (argc > 0) ? argv : NULL;
}

/* core エンジン (kw_core_wasi.c) が args/environ を読むためのアクセサ。 */
int wasi_get_argc(void)
{
	return wasi_argc;
}
const char* const* wasi_get_argv(void)
{
	return wasi_argv;
}
const char* const* wasi_get_envp(void)
{
	return wasi_envp;
}

void wasi_set_environ(const char* const* envp)
{
	wasi_envp = envp;
}

/* proc_exit の呼出有無と exit code を取り出す。
 * 戻り値: 1 = proc_exit が呼ばれた (out_code に値あり)、0 = 呼ばれていない。 */
int wasi_get_exit_status(int32_t* out_code)
{
	if(out_code != NULL)
		*out_code = wasi_exit_code;
	return wasi_exit_called;
}

/* 次の WASM 起動の前にリセットしたい場合に使う。 */
void wasi_reset_exit_status(void)
{
	wasi_exit_called = 0;
	wasi_exit_code   = 0;
}

/* envp の要素数 (NULL 終端まで) をカウント。 */
static size_t wasi_envp_count(void)
{
	if(wasi_envp == NULL)
		return 0;
	size_t n = 0;
	while(wasi_envp[n] != NULL)
		n++;
	return n;
}

/* ============================================================
 * VFS (Virtual File System) インタフェース
 * ============================================================
 *
 * WASI の path_open / fd_read / fd_seek 等は libc fopen/fread/fseek/fclose を
 * 直接呼ばず、この vtable 経由で動作する。利用者は wasi_set_vfs() で独自
 * 実装 (in-memory FS / 暗号化 FS / 圧縮アーカイブ展開等) を差し替えられる。
 *
 * open() の flags は将来 (Phase 3b) で書き込み機能を追加する余地として確保。
 * 現状は 0 = read-only のみ呼ばれる。
 * 各 op は NULL 不可 (デフォルト実装は libc fopen 系)。 */

#define WASI_VFS_OPEN_FLAG_READ   (1 << 0)
#define WASI_VFS_OPEN_FLAG_WRITE  (1 << 1)
#define WASI_VFS_OPEN_FLAG_CREATE (1 << 2)
#define WASI_VFS_OPEN_FLAG_TRUNC  (1 << 3)

typedef struct {
	uint64_t size;
	uint64_t atim_ns;
	uint64_t mtim_ns;
	uint64_t ctim_ns;
	uint8_t  filetype; /* __WASI_FILETYPE_* (0 = unknown) */
} wasi_vfs_filestat_t;

/* Phase 3c: directory enumeration entry。VFS は readdir_next で 1 件ずつ
 * この構造体を埋める。name は NUL 終端 (255 byte 上限、超えたら切り詰め)。 */
typedef struct {
	char name[260];
	uint8_t filetype; /* __WASI_FILETYPE_* */
} wasi_vfs_dirent_t;

typedef struct wasi_vfs {
	/* open: 成功時 opaque handle、失敗時 NULL。flags は WASI_VFS_OPEN_FLAG_*。 */
	void* (*open)(const char* host_path, int flags);
	void  (*close)(void* handle);
	/* read: 読み込んだ byte 数。EOF/エラーで 0。 */
	size_t (*read)(void* handle, void* buf, size_t count);
	/* seek: 新 offset を返す。失敗時 -1。whence は SEEK_SET/CUR/END (libc 互換)。 */
	int64_t (*seek)(void* handle, int64_t offset, int whence);
	/* tell: 現在 offset。失敗時 -1。 */
	int64_t (*tell)(void* handle);
	/* filestat: 0 = success、-1 = エラー (EBADF 相当)。 */
	int (*filestat)(const char* host_path, wasi_vfs_filestat_t* out);

	/* Phase 3b で追加された書き込み系 op。NULL ならランタイムは ENOSYS を返す。
	 * write: 書き込んだ byte 数。エラー時は count 未満を返す。 */
	size_t (*write)(void* handle, const void* buf, size_t count);
	/* unlink/mkdir/rmdir/rename: 0 = success、-1 = エラー。 */
	int (*unlink)(const char* host_path);
	int (*mkdir)(const char* host_path);
	int (*rmdir)(const char* host_path);
	int (*rename)(const char* from_path, const char* to_path);

	/* Phase 3c で追加。NULL ならランタイムは ENOSYS を返す。
	 * opendir: directory を列挙用に開く。NULL = エラー。 */
	void* (*opendir)(const char* host_path);
	void  (*closedir)(void* dh);
	/* readdir_next: 1 = entry を埋めた、0 = 末尾、-1 = エラー。
	 * "." / ".." はスキップして上位に渡さないこと。 */
	int   (*readdir_next)(void* dh, wasi_vfs_dirent_t* out);
	/* link/symlink: 0 = success、-1 = エラー。 */
	int   (*link)(const char* from_path, const char* to_path);
	int   (*symlink)(const char* target, const char* link_path);
	/* readlink: 書き込んだ byte 数 (NUL 終端なし)。エラー時 -1、リンク以外 -2。 */
	int64_t (*readlink)(const char* host_path, char* buf, size_t buf_len);

	/* realpath: host_path の symlink/junction を解決した正規化絶対パスを out へ書く。
	 * 0 = success、-1 = 失敗 (未存在 / buffer 不足 等)。NULL の場合ランタイムは
	 * symlink containment 検査をスキップする (mock VFS 等は自前で安全性を担保すること)。 */
	int (*realpath)(const char* host_path, char* out, size_t out_len);

	/* 任意の closure 用 user data (実装側で自由に使用可)。 */
	void* user_data;
} wasi_vfs_t;

/* #5: symlink containment 用の正規化パスバッファ長。Windows の \\?\ プレフィックス付き
 * 正規化パスや POSIX realpath の結果を収める。これを超える長さは containment 失敗扱い。
 * POSIX 版 libc_vfs_realpath が使うため、プラットフォーム別 VFS 実装より前で定義する。 */
#define WASI_REALPATH_MAX 4096

/* libc fopen 系を使ったデフォルト実装。
 * fopen の mode 文字列マッピング:
 *   read only             -> "rb"
 *   read+write 既存       -> "rb+"
 *   read+write CREATE/TRUNC -> "wb+" (ファイルが無ければ作成、あれば truncate)
 *   write only CREATE/TRUNC -> "wb" */
static void* libc_vfs_open(const char* host_path, int flags)
{
	const int has_read = (flags & WASI_VFS_OPEN_FLAG_READ) != 0;
	const int has_write = (flags & WASI_VFS_OPEN_FLAG_WRITE) != 0;
	const int has_create = (flags & (WASI_VFS_OPEN_FLAG_CREATE | WASI_VFS_OPEN_FLAG_TRUNC)) != 0;
	const char* mode;
	if(!has_write)
		mode = "rb";
	else if(has_read && has_create)
		mode = "wb+";
	else if(has_read)
		mode = "rb+";
	else if(has_create)
		mode = "wb";
	else
		mode = "rb+"; /* write-only on existing file */
	return fopen(host_path, mode);
}

static void libc_vfs_close(void* handle)
{
	if(handle != NULL)
		fclose((FILE*)handle);
}

static size_t libc_vfs_read(void* handle, void* buf, size_t count)
{
	return fread(buf, 1, count, (FILE*)handle);
}

static int64_t libc_vfs_seek(void* handle, int64_t offset, int whence)
{
#if defined(_WIN32)
	if(_fseeki64((FILE*)handle, offset, whence) != 0)
		return -1;
	return _ftelli64((FILE*)handle);
#else
	if(fseek((FILE*)handle, (long)offset, whence) != 0)
		return -1;
	return (int64_t)ftell((FILE*)handle);
#endif
}

static int64_t libc_vfs_tell(void* handle)
{
#if defined(_WIN32)
	return _ftelli64((FILE*)handle);
#else
	return (int64_t)ftell((FILE*)handle);
#endif
}

static int libc_vfs_filestat(const char* host_path, wasi_vfs_filestat_t* out)
{
#if defined(_WIN32)
	struct __stat64 st;
	if(_stat64(host_path, &st) != 0)
		return -1;
#else
	struct stat st;
	if(stat(host_path, &st) != 0)
		return -1;
#endif
	memset(out, 0, sizeof(*out));
	out->size = (uint64_t)st.st_size;
	out->atim_ns = (uint64_t)st.st_atime * 1000000000ULL;
	out->mtim_ns = (uint64_t)st.st_mtime * 1000000000ULL;
	out->ctim_ns = (uint64_t)st.st_ctime * 1000000000ULL;
	/* filetype 簡易判定: directory or regular。__WASI_FILETYPE_DIRECTORY=3, REGULAR_FILE=4 */
#if defined(_WIN32)
	out->filetype = (st.st_mode & _S_IFDIR) ? __WASI_FILETYPE_DIRECTORY : __WASI_FILETYPE_REGULAR_FILE;
#else
	out->filetype = S_ISDIR(st.st_mode) ? __WASI_FILETYPE_DIRECTORY : __WASI_FILETYPE_REGULAR_FILE;
#endif
	return 0;
}

static size_t libc_vfs_write(void* handle, const void* buf, size_t count)
{
	return fwrite(buf, 1, count, (FILE*)handle);
}

static int libc_vfs_unlink(const char* host_path)
{
#if defined(_WIN32)
	return _unlink(host_path);
#else
	return unlink(host_path);
#endif
}

static int libc_vfs_mkdir(const char* host_path)
{
#if defined(_WIN32)
	return _mkdir(host_path);
#else
	return mkdir(host_path, 0755);
#endif
}

static int libc_vfs_rmdir(const char* host_path)
{
#if defined(_WIN32)
	return _rmdir(host_path);
#else
	return rmdir(host_path);
#endif
}

static int libc_vfs_rename(const char* from_path, const char* to_path)
{
	return rename(from_path, to_path);
}

/* ----- Phase 3c: directory enumeration / link 系のデフォルト実装 ----- */

#if defined(_WIN32)
typedef struct {
	HANDLE find_handle;
	WIN32_FIND_DATAA find_data;
	int has_pending; /* FindFirstFile 直後の最初のエントリを保持しているか */
} libc_dir_t;

static void* libc_vfs_opendir(const char* host_path)
{
	size_t len = strlen(host_path);
	/* FindFirstFile 用の検索パターン "<path>\*" を構築する。 */
	char* pattern = (char*)malloc(len + 3);
	if(pattern == NULL)
		return NULL;
	memcpy(pattern, host_path, len);
	pattern[len] = '\\';
	pattern[len + 1] = '*';
	pattern[len + 2] = '\0';

	libc_dir_t* d = (libc_dir_t*)malloc(sizeof(*d));
	if(d == NULL) {
		free(pattern);
		return NULL;
	}
	d->find_handle = FindFirstFileA(pattern, &d->find_data);
	free(pattern);
	if(d->find_handle == INVALID_HANDLE_VALUE) {
		free(d);
		return NULL;
	}
	d->has_pending = 1;
	return d;
}

static void libc_vfs_closedir(void* dh)
{
	if(dh == NULL)
		return;
	libc_dir_t* d = (libc_dir_t*)dh;
	if(d->find_handle != INVALID_HANDLE_VALUE)
		FindClose(d->find_handle);
	free(d);
}

static int libc_vfs_readdir_next(void* dh, wasi_vfs_dirent_t* out)
{
	libc_dir_t* d = (libc_dir_t*)dh;
	for(;;) {
		if(!d->has_pending) {
			if(!FindNextFileA(d->find_handle, &d->find_data))
				return 0;
		}
		d->has_pending = 0;
		const char* name = d->find_data.cFileName;
		/* "." / ".." は除外 */
		if(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
			continue;
		size_t nlen = strlen(name);
		if(nlen >= sizeof(out->name))
			nlen = sizeof(out->name) - 1;
		memcpy(out->name, name, nlen);
		out->name[nlen] = '\0';
		uint8_t ft = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			? __WASI_FILETYPE_DIRECTORY : __WASI_FILETYPE_REGULAR_FILE;
		if(d->find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			ft = __WASI_FILETYPE_SYMBOLIC_LINK;
		out->filetype = ft;
		return 1;
	}
}

static int libc_vfs_link(const char* from_path, const char* to_path)
{
	/* 第一引数が new link、第二引数が existing target。 */
	return CreateHardLinkA(to_path, from_path, NULL) ? 0 : -1;
}

static int libc_vfs_symlink(const char* target, const char* link_path)
{
	/* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE = 0x2 (Win10 1703+, dev mode) */
	DWORD flags = 0x2;
	/* target が既存 directory なら DIRECTORY フラグを足す */
	DWORD attr = GetFileAttributesA(target);
	if(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
		flags |= 0x1; /* SYMBOLIC_LINK_FLAG_DIRECTORY */
	return CreateSymbolicLinkA(link_path, target, flags) ? 0 : -1;
}

/* FSCTL_GET_REPARSE_POINT 経由で symlink target を読み出す。
 * substitute name 部分 (PrintName) を最大 buf_len まで書き込み、書き込んだ
 * byte 数を返す。reparse point でない場合は -2 を返す。 */
typedef struct {
	ULONG  ReparseTag;
	USHORT ReparseDataLength;
	USHORT Reserved;
	USHORT SubstituteNameOffset;
	USHORT SubstituteNameLength;
	USHORT PrintNameOffset;
	USHORT PrintNameLength;
	ULONG  Flags;
	WCHAR  PathBuffer[1];
} libc_symlink_reparse_t;

static int64_t libc_vfs_readlink(const char* host_path, char* buf, size_t buf_len)
{
	DWORD attr = GetFileAttributesA(host_path);
	if(attr == INVALID_FILE_ATTRIBUTES)
		return -1;
	if(!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
		return -2;

	HANDLE h = CreateFileA(host_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return -1;

	uint8_t reparse_buf[16384]; /* MAXIMUM_REPARSE_DATA_BUFFER_SIZE */
	DWORD bytes_returned = 0;
	if(!DeviceIoControl(h, 0x000900A8 /* FSCTL_GET_REPARSE_POINT */, NULL, 0,
		reparse_buf, sizeof(reparse_buf), &bytes_returned, NULL)) {
		CloseHandle(h);
		return -1;
	}
	CloseHandle(h);

	libc_symlink_reparse_t* rep = (libc_symlink_reparse_t*)reparse_buf;
	/* ヘッダ分のデータが返っていることを先に確認 (未初期化フィールド読みの防止)。 */
	const size_t rep_hdr = offsetof(libc_symlink_reparse_t, PathBuffer);
	if(bytes_returned < rep_hdr)
		return -1;
	if(rep->ReparseTag != 0xA000000C /* IO_REPARSE_TAG_SYMLINK */)
		return -2;

	/* PathBuffer は WCHAR、PrintName を採用。 */
	WCHAR* base = rep->PathBuffer;
	int name_off_chars = rep->PrintNameOffset / 2;
	int name_len_chars = rep->PrintNameLength / 2;
	if(name_len_chars <= 0) {
		/* fallback: SubstituteName */
		name_off_chars = rep->SubstituteNameOffset / 2;
		name_len_chars = rep->SubstituteNameLength / 2;
	}
	if(name_len_chars <= 0)
		return 0;
	/* セキュリティ: offset/length は FS 由来の値をそのまま信頼せず、DeviceIoControl が実際に
	 * 返したデータ長の範囲内であることを検証する (細工された reparse point による
	 * reparse_buf 外=スタック外読み出し→ゲストへの情報漏洩を防ぐ)。 */
	size_t avail_chars = (bytes_returned - rep_hdr) / 2;
	if((size_t)name_off_chars + (size_t)name_len_chars > avail_chars)
		return -1;

	int written = WideCharToMultiByte(CP_UTF8, 0, base + name_off_chars, name_len_chars,
		buf, (int)buf_len, NULL, NULL);
	/* WideCharToMultiByte は失敗時 (buf_len 不足・変換不能) に 0 を返す (負値は返さない)。
	 * name_len_chars > 0 はここまでで保証済みなので、成功なら必ず written > 0。written == 0 は
	 * 失敗なので弾く (0 を成功として返すと空 target を正常読取と誤認する)。 */
	if(written <= 0)
		return -1;

	return (int64_t)written;
}

#elif defined(CONSOLE_PLATFORM) /* 家庭用機 */

/* 家庭用機向け WASI libc 既定 VFS は全機能未対応のスタブ。
 * 本来 WASI 系 API は WASM 製ツール用途を想定しており、ゲーム本編は
 * MeEngine の API 経由 (wasi_vfs_set による差し替え) でファイルアクセスする。
 * ハードリンク / シンボリックリンク / ディレクトリ列挙は SDK が提供しないため
 * 「サポート外」を返すだけのダミー実装にする。 */

static void* libc_vfs_opendir(const char* host_path)
{
	(void)host_path;
	return NULL;
}

static void libc_vfs_closedir(void* dh)
{
	(void)dh;
}

static int libc_vfs_readdir_next(void* dh, wasi_vfs_dirent_t* out)
{
	(void)dh;
	(void)out;
	return 0; /* EOF (空ディレクトリ相当) */
}

static int libc_vfs_link(const char* from_path, const char* to_path)
{
	(void)from_path;
	(void)to_path;
	return -1;
}

static int libc_vfs_symlink(const char* target, const char* link_path)
{
	(void)target;
	(void)link_path;
	return -1;
}

static int64_t libc_vfs_readlink(const char* host_path, char* buf, size_t buf_len)
{
	(void)host_path;
	(void)buf;
	(void)buf_len;
	return -2; /* "シンボリックリンクではない" センチネル */
}

#else /* POSIX */

#include <dirent.h>

static void* libc_vfs_opendir(const char* host_path)
{
	return opendir(host_path);
}

static void libc_vfs_closedir(void* dh)
{
	if(dh != NULL)
		closedir((DIR*)dh);
}

static int libc_vfs_readdir_next(void* dh, wasi_vfs_dirent_t* out)
{
	DIR* d = (DIR*)dh;
	for(;;) {
		errno = 0;
		struct dirent* e = readdir(d);
		if(e == NULL)
			return (errno == 0) ? 0 : -1;
		const char* name = e->d_name;
		if(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
			continue;
		size_t nlen = strlen(name);
		if(nlen >= sizeof(out->name))
			nlen = sizeof(out->name) - 1;
		memcpy(out->name, name, nlen);
		out->name[nlen] = '\0';
		uint8_t ft = __WASI_FILETYPE_REGULAR_FILE;
#ifdef DT_DIR
		if(e->d_type == DT_DIR)
			ft = __WASI_FILETYPE_DIRECTORY;
		else if(e->d_type == DT_LNK)
			ft = __WASI_FILETYPE_SYMBOLIC_LINK;
		else if(e->d_type == DT_REG)
			ft = __WASI_FILETYPE_REGULAR_FILE;
		else if(e->d_type == DT_UNKNOWN)
			ft = __WASI_FILETYPE_UNKNOWN;
#endif
		out->filetype = ft;
		return 1;
	}
}

static int libc_vfs_link(const char* from_path, const char* to_path)
{
	return link(from_path, to_path);
}

static int libc_vfs_symlink(const char* target, const char* link_path)
{
	return symlink(target, link_path);
}

static int64_t libc_vfs_readlink(const char* host_path, char* buf, size_t buf_len)
{
	ssize_t r = readlink(host_path, buf, buf_len);
	if(r < 0) {
		if(errno == EINVAL)
			return -2;
		return -1;
	}
	return (int64_t)r;
}
#endif

/* libc_vfs_realpath — host_path の symlink/junction を解決した正規化絶対パスを out へ。
 * Windows: ファイル/ディレクトリを開いて GetFinalPathNameByHandleA で実体パスを取得
 * (reparse point を follow した先の正規パス)。POSIX: realpath()。
 * CONSOLE_PLATFORM は実ファイル非対応のため未提供 (containment skip)。 */
#if defined(_WIN32)
static int libc_vfs_realpath(const char* host_path, char* out, size_t out_len)
{
	HANDLE h = CreateFileA(host_path, 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return -1;
	/* 戻り値は書いた文字数 (NUL 除く)。out_len 以上を要する場合は必要長を返すので失敗扱い。 */
	DWORD n = GetFinalPathNameByHandleA(h, out, (DWORD)out_len,
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(h);
	if(n == 0 || n >= out_len)
		return -1;
	return 0;
}
#elif !defined(CONSOLE_PLATFORM)
static int libc_vfs_realpath(const char* host_path, char* out, size_t out_len)
{
	char tmp[WASI_REALPATH_MAX];
	if(realpath(host_path, tmp) == NULL)
		return -1;
	size_t len = strlen(tmp);
	if(len >= out_len)
		return -1;
	memcpy(out, tmp, len + 1);
	return 0;
}
#endif

static const wasi_vfs_t default_vfs = {
	.open = libc_vfs_open,
	.close = libc_vfs_close,
	.read = libc_vfs_read,
	.seek = libc_vfs_seek,
	.tell = libc_vfs_tell,
	.filestat = libc_vfs_filestat,
	.write = libc_vfs_write,
	.unlink = libc_vfs_unlink,
	.mkdir = libc_vfs_mkdir,
	.rmdir = libc_vfs_rmdir,
	.rename = libc_vfs_rename,
	.opendir = libc_vfs_opendir,
	.closedir = libc_vfs_closedir,
	.readdir_next = libc_vfs_readdir_next,
	.link = libc_vfs_link,
	.symlink = libc_vfs_symlink,
	.readlink = libc_vfs_readlink,
#if !defined(CONSOLE_PLATFORM)
	.realpath = libc_vfs_realpath,
#endif
	.user_data = NULL,
};

static const wasi_vfs_t* wasi_vfs = &default_vfs;

/* ホストから VFS を差し替える公開 API。NULL を渡すとデフォルト (libc) に戻す。
 * 渡された vtable は呼び出し側が所有 (本コードは複製しない、参照のみ保持)。
 * 切替時に既存の open 済 handle はそのまま (古い VFS の close を後で呼べる
 * 必要があるため)、利用側で適切なライフタイム管理を行うこと。 */
void wasi_set_vfs(const wasi_vfs_t* vfs)
{
	wasi_vfs = (vfs != NULL) ? vfs : &default_vfs;
}

/* ============================================================
 * WASI ファイルディスクリプタ管理
 * ============================================================ */

/* fd 0/1/2 は stdin/stdout/stderr で固定。
 * fd 3.. は wasi_fds[fd - WASI_FD_BASE] にマップする。
 * preopened directory は wasi_add_preopen 呼び出し順に slot 0, 1, ... を占有。
 * path_open はそれより後ろの slot を使う。 */
#define WASI_FD_BASE 3
#define WASI_FD_MAX 64
#define WASI_MAX_PREOPENS 8
#define WASI_PATH_MAX 1024
/* fd_read/write/pread/pwrite の iovs 数上限。
 * spec では明示的上限なしだが、巨大値による DoS を防ぐため現実的な値で制限。
 * 通常 wasi-libc は 1〜数個の iovec しか使わない。 */
#define WASI_IOVEC_MAX 1024

/* セキュリティ #17: fd_read/write/pread/pwrite の per-iovec count や random_get の
 * buf_len は wasm 側が制御する u32 (最大 4GB)。そのまま malloc すると単一呼出しで
 * 数 GB のホスト確保を強制でき RAM 枯渇 DoS になる。staging バッファをこの上限で
 * 頭打ちにし、超過分はチャンク分割して転送する (転送バイト数・EOF 判定は不変)。 */
#define WASI_STAGE_MAX (16u * 1024u * 1024u)

typedef struct {
	char* host_path; /* ホスト側 path (allocated) */
	char* wasi_name; /* WASI 側公開名 (allocated) */
} wasi_preopen_t;

typedef struct {
	uint8_t is_used;
	uint8_t is_preopen; /* 1: preopened directory, 0: 通常 file */
	uint8_t filetype; /* __WASI_FILETYPE_* */
	void* handle; /* VFS open() が返した opaque handle (preopen は NULL) */
	char* host_path; /* 開いたファイルの host path (preopen は preopens[idx].host_path を共有) */
	int preopen_idx; /* is_preopen=1 のとき preopens[] のインデックス */
} wasi_fd_t;

static wasi_preopen_t wasi_preopens[WASI_MAX_PREOPENS];
static int wasi_preopen_count = 0;
static wasi_fd_t wasi_fds[WASI_FD_MAX];

static wasi_fd_t* wasi_fd_get(int32_t fd)
{
	if(fd < WASI_FD_BASE)
		return NULL;
	int idx = fd - WASI_FD_BASE;
	if(idx >= WASI_FD_MAX)
		return NULL;
	if(!wasi_fds[idx].is_used)
		return NULL;
	return &wasi_fds[idx];
}

/* path_open 用に空き slot を確保 (preopen 領域より後ろ) */
static int32_t wasi_fd_alloc(void)
{
	for(int i = wasi_preopen_count; i < WASI_FD_MAX; i++) {
		if(!wasi_fds[i].is_used)
			return WASI_FD_BASE + i;
	}
	return -1;
}

/* ホスト側から preopened directory を登録する公開 API。
 * 戻り値: 成功時 WASI fd 番号 (3+)、失敗時 -1。
 * host_path / wasi_name は内部で複製して保持する。 */
int32_t wasi_add_preopen(const char* host_path, const char* wasi_name)
{
	if(host_path == NULL || wasi_name == NULL)
		return -1;
	if(wasi_preopen_count >= WASI_MAX_PREOPENS)
		return -1;

	int idx = wasi_preopen_count;
	size_t hp_len = strlen(host_path);
	size_t wn_len = strlen(wasi_name);
	char* hp = (char*)malloc(hp_len + 1);
	char* wn = (char*)malloc(wn_len + 1);
	if(hp == NULL || wn == NULL) {
		free(hp);
		free(wn);
		return -1;
	}
	memcpy(hp, host_path, hp_len + 1);
	memcpy(wn, wasi_name, wn_len + 1);

	wasi_preopens[idx].host_path = hp;
	wasi_preopens[idx].wasi_name = wn;
	wasi_fds[idx].is_used = 1;
	wasi_fds[idx].is_preopen = 1;
	wasi_fds[idx].filetype = __WASI_FILETYPE_DIRECTORY;
	wasi_fds[idx].handle = NULL;
	wasi_fds[idx].host_path = hp;
	wasi_fds[idx].preopen_idx = idx;
	wasi_preopen_count++;
	return WASI_FD_BASE + idx;
}

/* 全 preopen + 開いたファイルを閉じてリセット。テスト用途。 */
void wasi_clear_preopens(void)
{
	for(int i = 0; i < WASI_FD_MAX; i++) {
		if(wasi_fds[i].is_used && !wasi_fds[i].is_preopen) {
			if(wasi_fds[i].handle != NULL)
				wasi_vfs->close(wasi_fds[i].handle);
			free(wasi_fds[i].host_path);
		}
		memset(&wasi_fds[i], 0, sizeof(wasi_fds[i]));
	}
	for(int i = 0; i < wasi_preopen_count; i++) {
		free(wasi_preopens[i].host_path);
		free(wasi_preopens[i].wasi_name);
		memset(&wasi_preopens[i], 0, sizeof(wasi_preopens[i]));
	}
	wasi_preopen_count = 0;
}

/* path 文字列に ".." または絶対パス特徴がないか検証する。
 * 受理: "foo", "foo/bar", "foo/./bar"
 * 拒否: "..", "../foo", "foo/..", "foo/../bar", "/foo", "foo//bar" 等 */
#if defined(_WIN32)
/* Windows の予約 device 名 (case-insensitive)。拡張子付きでも device に
 * 解決されるため、basename (最初の '.' まで) を厳密比較する。
 * con/prn/aux/nul/com1-9/lpt1-9 を対象 (WASI 仕様外だが host 安全のため)。*/
static int wasi_segment_is_windows_reserved(const char* seg, size_t seg_len)
{
	/* basename = seg の先頭から '.' まで */
	size_t base_len = seg_len;
	for(size_t i = 0; i < seg_len; i++) {
		if(seg[i] == '.') {
			base_len = i;
			break;
		}
	}
	if(base_len == 3) {
		char b[3];
		for(size_t i = 0; i < 3; i++) {
			char c = seg[i];
			b[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
		}
		if((b[0] == 'c' && b[1] == 'o' && b[2] == 'n') ||
		    (b[0] == 'p' && b[1] == 'r' && b[2] == 'n') ||
		    (b[0] == 'a' && b[1] == 'u' && b[2] == 'x') ||
		    (b[0] == 'n' && b[1] == 'u' && b[2] == 'l'))
			return 1;
	} else if(base_len == 4) {
		char b[4];
		for(size_t i = 0; i < 4; i++) {
			char c = seg[i];
			b[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
		}
		int is_com = (b[0] == 'c' && b[1] == 'o' && b[2] == 'm');
		int is_lpt = (b[0] == 'l' && b[1] == 'p' && b[2] == 't');
		if((is_com || is_lpt) && b[3] >= '1' && b[3] <= '9')
			return 1;
	}
	return 0;
}
#endif

static int wasi_path_is_safe(const char* path, size_t len)
{
	if(len == 0)
		return 1; /* 空 path は preopen 自身を指すので許可 */
	if(path[0] == '/' || path[0] == '\\')
		return 0; /* 絶対パス拒否 */
	size_t seg_start = 0;
	for(size_t i = 0; i <= len; i++) {
		if(i == len || path[i] == '/' || path[i] == '\\') {
			size_t seg_len = i - seg_start;
			if(seg_len == 0) {
				/* "//" のような空セグメントは拒否 (末尾 / は除く) */
				if(i != len)
					return 0;
			} else if(seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
				return 0;
			}
#if defined(_WIN32)
			else if(seg_len > 0 && wasi_segment_is_windows_reserved(path + seg_start, seg_len)) {
				return 0;
			}
#endif
			seg_start = i + 1;
		} else if(path[i] == ':') {
			/* #37: ':' は WASI 相対 path に不正。Windows の drive-relative ('C:foo') や
			 * NTFS Alternate Data Stream ('file:stream') による sandbox 逸脱を防ぐ。 */
			return 0;
		}
	}
	return 1;
}

/* wasi_path_contained: 組み立てた host path `full` の実パスが、capability の基点 `base`
 * (preopen / dir fd の host path) の実パス subtree 内に収まるかを検証する (#5)。
 * fopen/stat は symlink/junction を follow するため、wasi_path_is_safe の字句検査だけでは
 * preopen 内に置かれた「外部を指す既存 symlink」や「symlink ディレクトリ成分」経由の
 * 逸脱を防げない。realpath で双方を解決し、RESOLVE_BENEATH 相当の境界検査を行う。
 * VFS が realpath を提供しない (mock VFS 等) 場合は検査をスキップ (従来動作)。
 * 戻り値: 1 = 収まる (許可) / 0 = 逸脱 or 解決不能 (拒否)。
 * 注: realpath→実アクセスの間の TOCTOU は残存 (handle ベース検証は別途要)。 */
static int wasi_path_contained(const char* base, char* full)
{
	if(wasi_vfs->realpath == NULL)
		return 1;
	char rbase[WASI_REALPATH_MAX];
	char rfull[WASI_REALPATH_MAX];
	if(wasi_vfs->realpath(base, rbase, sizeof(rbase)) != 0)
		return 0; /* 基点が解決できない = 異常、拒否 */
	if(wasi_vfs->realpath(full, rfull, sizeof(rfull)) != 0) {
		/* full が未存在 (O_CREAT / mkdir / symlink new_path 等): 親ディレクトリで検査。
		 * leaf は wasi_path_is_safe 済み (絶対/.. 無し) なので親が収まれば leaf も収まる。
		 * leaf が既存 symlink なら上の realpath(full) が成功するためここには来ない。 */
		size_t cut = strlen(full);
		while(cut > 0 && full[cut - 1] != '/' && full[cut - 1] != '\\')
			cut--;
		if(cut == 0)
			return 0;
		char saved = full[cut - 1];
		full[cut - 1] = '\0';
		int ok = (wasi_vfs->realpath(full, rfull, sizeof(rfull)) == 0);
		full[cut - 1] = saved; /* 復元 */
		if(!ok)
			return 0;
	}
	size_t blen = strlen(rbase);
	if(strncmp(rfull, rbase, blen) != 0)
		return 0;
	/* 境界: rfull == rbase か、直後がセパレータ (rbase の prefix-sibling を弾く)。 */
	return (rfull[blen] == '\0' || rfull[blen] == '/' || rfull[blen] == '\\');
}

/* path_open: preopened directory 配下のファイルを開く。
 * 引数: dirfd, dirflags, path_addr, path_len, oflags, fs_rights_base (i64),
 *       fs_rights_inheriting (i64), fdflags, opened_fd_out_ptr
 * path に ".." / 絶対パスを含むものは EINVAL で拒否 (capability sandbox)。
 * fs_rights_base に WRITE が含まれていれば書き込みモードで開く。 */
static kinowasm_result_t path_open(api_call_t api_call)
{
	int32_t dirfd = GETPARAM_INT(0);
	(void)GETPARAM_INT(1); /* dirflags (LOOKUP_SYMLINK_FOLLOW): 現状未使用 */
	uint32_t path_addr = (uint32_t)GETPARAM_INT(2);
	uint32_t path_len = (uint32_t)GETPARAM_INT(3);
	int32_t oflags = GETPARAM_INT(4);
	int64_t fs_rights_base = api_call->args->data[5].val.num.i64;
	(void)api_call->args->data[6].val.num.i64; /* fs_rights_inheriting */
	(void)GETPARAM_INT(7); /* fdflags */
	uint32_t opened_fd_out_ptr = (uint32_t)GETPARAM_INT(8);

	wasi_fd_t* dir_entry = wasi_fd_get(dirfd);
	if(dir_entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	/* 受理する dirfd: preopen か、path_open(OFLAGS_DIRECTORY) で得た directory fd。
	 * それ以外 (REGULAR_FILE 等を dirfd に指定された) は ENOTDIR。 */
	if(!dir_entry->is_preopen && dir_entry->filetype != __WASI_FILETYPE_DIRECTORY) {
		SETRET_INT(0, __WASI_ENOTDIR);
		return 0;
	}

	if(path_len > WASI_PATH_MAX) {
		SETRET_INT(0, __WASI_EOVERFLOW);
		return 0;
	}

	char path[WASI_PATH_MAX + 1];
	/* セキュリティ: 読み取り失敗 (線形メモリ範囲外) 時は path が未初期化のままなので、
	 * 安全検査/パス組み立てに渡さず EFAULT を返す (read_memory は all-or-nothing)。 */
	if(path_len > 0 && kinowasm_read_memory(api_call, path_addr, path, path_len) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	path[path_len] = '\0';

	if(!wasi_path_is_safe(path, path_len)) {
		/* path traversal を含むので拒否 */
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	/* host_path = (dir base) + "/" + path */
	const char* base = dir_entry->is_preopen
		? wasi_preopens[dir_entry->preopen_idx].host_path
		: dir_entry->host_path;
	if(base == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	size_t base_len = strlen(base);
	/* base_len + path_len + 2 が size_t overflow するのを防ぐ。
	 * preopen 登録時に異常な長さの host_path が渡されたケースの保険。
	 * path_len は line 890 で WASI_PATH_MAX を上回らないことを確認済み。 */
	if(base_len > SIZE_MAX - WASI_PATH_MAX - 2) {
		SETRET_INT(0, __WASI_EOVERFLOW);
		return 0;
	}
	size_t total = base_len + 1 + path_len + 1;
	char* full = (char*)malloc(total);
	if(full == NULL) {
		SETRET_INT(0, __WASI_ENOMEM);
		return 0;
	}
	memcpy(full, base, base_len);
	full[base_len] = '/';
	memcpy(full + base_len + 1, path, path_len);
	full[base_len + 1 + path_len] = '\0';

	/* #5: symlink/junction を解決した実パスが base subtree 内に収まることを検証。 */
	if(!wasi_path_contained(base, full)) {
		free(full);
		SETRET_INT(0, __WASI_EACCES);
		return 0;
	}

	void* handle = NULL;
	uint8_t filetype = __WASI_FILETYPE_REGULAR_FILE;

	if(oflags & __WASI_OFLAGS_DIRECTORY) {
		/* directory として開く: handle は持たず host_path のみ保持。
		 * 実体が存在し directory であることを filestat で検証する。 */
		wasi_vfs_filestat_t fs;
		if(wasi_vfs->filestat == NULL || wasi_vfs->filestat(full, &fs) != 0) {
			free(full);
			SETRET_INT(0, __WASI_ENOENT);
			return 0;
		}
		if(fs.filetype != __WASI_FILETYPE_DIRECTORY) {
			free(full);
			SETRET_INT(0, __WASI_ENOTDIR);
			return 0;
		}
		filetype = __WASI_FILETYPE_DIRECTORY;
	} else {
		/* O_EXCL: CREAT と組み合わせて、対象が既に存在する場合は EEXIST。
		 * (__WASI_OFLAGS_EXCL = 4) */
		if((oflags & 4) && (oflags & __WASI_OFLAGS_CREAT)) {
			wasi_vfs_filestat_t fs;
			if(wasi_vfs->filestat != NULL && wasi_vfs->filestat(full, &fs) == 0) {
				free(full);
				SETRET_INT(0, __WASI_EEXIST);
				return 0;
			}
		}
		int vfs_flags = 0;
		/* fs_rights_base が空ならデフォルトで READ を有効化 (古いコンパイラ出力対応)。 */
		if((fs_rights_base & __WASI_RIGHT_FD_READ) || fs_rights_base == 0)
			vfs_flags |= WASI_VFS_OPEN_FLAG_READ;
		if(fs_rights_base & __WASI_RIGHT_FD_WRITE)
			vfs_flags |= WASI_VFS_OPEN_FLAG_WRITE;
		if(oflags & __WASI_OFLAGS_CREAT)
			vfs_flags |= WASI_VFS_OPEN_FLAG_CREATE;
		if(oflags & __WASI_OFLAGS_TRUNC)
			vfs_flags |= WASI_VFS_OPEN_FLAG_TRUNC;
		/* CREATE / TRUNC が要求されている場合は WRITE が暗黙で必要。
		 * fs_rights_base に明示が無くても open に失敗しないよう補正する。 */
		if(vfs_flags & (WASI_VFS_OPEN_FLAG_CREATE | WASI_VFS_OPEN_FLAG_TRUNC))
			vfs_flags |= WASI_VFS_OPEN_FLAG_WRITE;

		errno = 0;
		handle = wasi_vfs->open(full, vfs_flags);
		if(handle == NULL) {
			/* fopen 失敗時は host errno を見て ENOENT/EACCES/EISDIR 等を返す。
			 * VFS 実装が errno をセットしない場合は ENOENT を仮定 (mock vfs 等)。 */
			uint32_t err = (errno != 0) ? wasi_errno_from_host(errno) : __WASI_ENOENT;
			free(full);
			SETRET_INT(0, err);
			return 0;
		}
	}

	int32_t new_fd = wasi_fd_alloc();
	if(new_fd < 0) {
		if(handle != NULL)
			wasi_vfs->close(handle);
		free(full);
		SETRET_INT(0, __WASI_ENOMEM);
		return 0;
	}

	int idx = new_fd - WASI_FD_BASE;
	wasi_fds[idx].is_used = 1;
	wasi_fds[idx].is_preopen = 0;
	wasi_fds[idx].filetype = filetype;
	wasi_fds[idx].handle = handle;
	wasi_fds[idx].host_path = full;
	wasi_fds[idx].preopen_idx = -1;

	uint32_t fd_u32 = (uint32_t)new_fd;
	/* セキュリティ: 書き込み先が線形メモリ範囲外だとゲストは fd 番号を知れず fd_close 不能になり
	 * slot/handle がリークするため、割り当てを巻き戻して EFAULT を返す。 */
	if(kinowasm_write_memory(api_call, opened_fd_out_ptr, &fd_u32, sizeof(fd_u32)) != RES_SUCCESS) {
		if(handle != NULL)
			wasi_vfs->close(handle);
		free(full);
		memset(&wasi_fds[idx], 0, sizeof(wasi_fds[idx]));
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_close: 0/1/2 は no-op、preopen は閉じない (永続)、通常 file は fclose。 */
static kinowasm_result_t fd_close(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	if(fd >= 0 && fd <= 2) {
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}
	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	if(entry->is_preopen) {
		/* preopen は閉じない (永続)。 */
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}
	if(entry->handle != NULL)
		wasi_vfs->close(entry->handle);
	free(entry->host_path);
	memset(entry, 0, sizeof(*entry));
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_seek: file fd の offset を移動し、新 offset を newoffset_ptr に書く。
 * stdin/stdout/stderr / preopen / directory は ESPIPE。 */
static kinowasm_result_t fd_seek(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	int64_t offset = api_call->args->data[1].val.num.i64;
	int32_t whence = GETPARAM_INT(2);
	uint32_t newoffset_ptr = (uint32_t)GETPARAM_INT(3);

	if(fd >= 0 && fd <= 2) {
		SETRET_INT(0, __WASI_ESPIPE);
		return 0;
	}
	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	/* directory に対する fd_seek は EISDIR (NOTCAPABLE 系の代替として OK)。 */
	if(entry->is_preopen || entry->filetype == __WASI_FILETYPE_DIRECTORY) {
		SETRET_INT(0, __WASI_EISDIR);
		return 0;
	}
	if(entry->handle == NULL) {
		SETRET_INT(0, __WASI_ESPIPE);
		return 0;
	}

	int origin;
	switch(whence) {
	case __WASI_WHENCE_SET:
		origin = SEEK_SET;
		break;
	case __WASI_WHENCE_CUR:
		origin = SEEK_CUR;
		break;
	case __WASI_WHENCE_END:
		origin = SEEK_END;
		break;
	default:
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	int64_t pos = wasi_vfs->seek(entry->handle, offset, origin);
	if(pos < 0) {
		SETRET_INT(0, __WASI_EIO);
		return 0;
	}
	uint64_t pos_u64 = (uint64_t)pos;
	/* 返却先が範囲外だとゲストは新しい offset を取得できない。seek (副作用) は完了済みで、
	 * SEEK_CUR ではゲストの再試行時に相対 seek が二重適用されうるが、out ポインタを事前検査
	 * する手段が無いため fd_read/fd_write と同様に WASI 仕様どおり EFAULT を返す。 */
	if(kinowasm_write_memory(api_call, newoffset_ptr, &pos_u64, sizeof(pos_u64)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_tell: 現在 offset。 */
static kinowasm_result_t fd_tell(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t out_ptr = (uint32_t)GETPARAM_INT(1);
	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || entry->is_preopen || entry->handle == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	int64_t pos = wasi_vfs->tell(entry->handle);
	if(pos < 0) {
		SETRET_INT(0, __WASI_EIO);
		return 0;
	}
	uint64_t pos_u64 = (uint64_t)pos;
	if(kinowasm_write_memory(api_call, out_ptr, &pos_u64, sizeof(pos_u64)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* iovec を 1 つ読んで host_buf に書く共通実装。
 * 戻り値: バイト数。stdin の場合は handle == NULL を渡し fread(stdin) を直接使う。 */
static uint32_t wasi_read_iovs_to_handle(api_call_t api_call, uint32_t iovs, int32_t len, void* handle, int is_stdin)
{
	uint32_t total = 0;
	int32_t off = 0;
	for(int32_t i = 0; i < len; i++) {
		uint32_t address_info[2];
		/* セキュリティ: iovec 配列自体が線形メモリ範囲外なら address_info が未初期化のまま
		 * (ptr/len が garbage) になるので、これ以上は信頼できず打ち切る (書き込み側 wasi_write_iovs_from_handle と対称)。 */
		if(kinowasm_read_memory(api_call, (uint32_t)(iovs + off), &address_info, sizeof(address_info)) != RES_SUCCESS)
			break;
		off += (int32_t)sizeof(address_info);
		uint32_t count = address_info[1];
		if(count == 0)
			continue;
		/* #17: staging を WASI_STAGE_MAX で頭打ちにし、iovec 内をチャンク分割して読む。 */
		uint32_t cap = count < WASI_STAGE_MAX ? count : WASI_STAGE_MAX;
		uint8_t* data = (uint8_t*)kinowasm_mem_malloc(cap);
		if(data == NULL)
			break;
		uint32_t addr = address_info[0];
		uint32_t remaining = count;
		int eof = 0;
		while(remaining > 0) {
			uint32_t chunk = remaining < cap ? remaining : cap;
			size_t got = is_stdin
				? fread(data, 1, chunk, stdin)
				: wasi_vfs->read(handle, data, chunk);
			if(got > 0) {
				/* セキュリティ: 転送先が線形メモリ範囲外なら書き込まず打ち切る (書き込み側と対称)。
				 * 読み出したバイトは消費済みだが total には数えないため、書けていない量を read 済みとして返さない。 */
				if(kinowasm_write_memory(api_call, addr, data, got) != RES_SUCCESS) {
					eof = 1;
					break;
				}
			}
			total += (uint32_t)got;
			addr += (uint32_t)got;
			remaining -= (uint32_t)got;
			if(got < chunk) { /* EOF / short read */
				eof = 1;
				break;
			}
		}
		kinowasm_mem_free(data);
		if(eof)
			break;
	}
	return total;
}

/* fd_read: stdin (fd=0) or 通常 file fd を iovec 経由で読む。
 * fd_write/fd_pwrite 共通: iovec を順に読み handle (is_stdio 時は stdio_fp) へ書き、
 * 書き込めたバイト数を返す。staging は WASI_STAGE_MAX で頭打ちにしチャンク分割する (#17)。 */
static uint32_t wasi_write_iovs_from_handle(api_call_t api_call, uint32_t iovs, int32_t len, void* handle, FILE* stdio_fp, int is_stdio)
{
	uint32_t total = 0;
	int32_t off = 0;
	for(int32_t i = 0; i < len; i++) {
		uint32_t address_info[2];
		/* セキュリティ: iovec 配列自体が線形メモリ範囲外なら address_info が未初期化のまま
		 * (ptr/len が garbage) になるので、これ以上は信頼できず打ち切る。 */
		if(kinowasm_read_memory(api_call, (uint32_t)(iovs + off), &address_info, sizeof(address_info)) != RES_SUCCESS)
			break;
		off += (int32_t)sizeof(address_info);
		uint32_t count = address_info[1];
		if(count == 0)
			continue;
		uint32_t cap = count < WASI_STAGE_MAX ? count : WASI_STAGE_MAX;
		uint8_t* data = (uint8_t*)kinowasm_mem_malloc(cap);
		if(data == NULL)
			break;
		uint32_t addr = address_info[0];
		uint32_t remaining = count;
		int short_write = 0;
		while(remaining > 0) {
			uint32_t chunk = remaining < cap ? remaining : cap;
			/* セキュリティ: 読み取り失敗 (線形メモリ範囲外) 時は data が未初期化 (kalloc は非ゼロ)
			 * のままなので、絶対に書き出さない (未初期化ヒープの stdout/ファイル漏洩を防ぐ)。 */
			if(kinowasm_read_memory(api_call, addr, data, chunk) != RES_SUCCESS) {
				short_write = 1;
				break;
			}
			size_t got = is_stdio
				? fwrite(data, 1, chunk, stdio_fp)
				: wasi_vfs->write(handle, data, chunk);
			total += (uint32_t)got;
			addr += (uint32_t)got;
			remaining -= (uint32_t)got;
			if(got < chunk) {
				short_write = 1;
				break;
			}
		}
		kinowasm_mem_free(data);
		if(short_write)
			break;
	}
	return total;
}

static kinowasm_result_t fd_read(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t iovs = (uint32_t)GETPARAM_INT(1);
	int32_t len = GETPARAM_INT(2);
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(3);

	if(len < 0 || len > WASI_IOVEC_MAX) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	void* handle = NULL;
	int is_stdin = 0;
	if(fd == 0) {
		is_stdin = 1;
	} else {
		wasi_fd_t* entry = wasi_fd_get(fd);
		if(entry == NULL || entry->is_preopen || entry->handle == NULL) {
			SETRET_INT(0, __WASI_EBADF);
			return 0;
		}
		handle = entry->handle;
	}

	uint32_t read_byte = wasi_read_iovs_to_handle(api_call, iovs, len, handle, is_stdin);
	/* 返却先が線形メモリ範囲外だとゲストは読取バイト数を知れず失敗と誤認して再試行しうる。
	 * 読取 (副作用) 自体は完了済みだが WASI 仕様どおり EFAULT を返す。 */
	if(kinowasm_write_memory(api_call, ret_ptr, &read_byte, sizeof(read_byte)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_pread: offset 指定 read。handle の位置は復元する。 */
static kinowasm_result_t fd_pread(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t iovs = (uint32_t)GETPARAM_INT(1);
	int32_t len = GETPARAM_INT(2);
	int64_t offset = api_call->args->data[3].val.num.i64;
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(4);

	if(len < 0 || len > WASI_IOVEC_MAX) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || entry->is_preopen || entry->handle == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}

	int64_t saved = wasi_vfs->tell(entry->handle);
	if(saved < 0 || wasi_vfs->seek(entry->handle, offset, SEEK_SET) < 0) {
		SETRET_INT(0, __WASI_EIO);
		return 0;
	}
	uint32_t read_byte = wasi_read_iovs_to_handle(api_call, iovs, len, entry->handle, 0);
	wasi_vfs->seek(entry->handle, saved, SEEK_SET);
	/* 位置は復元済み・読取は冪等なので、返却先不正は WASI 仕様どおり EFAULT を返す。 */
	if(kinowasm_write_memory(api_call, ret_ptr, &read_byte, sizeof(read_byte)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_write: stdout/stderr (fd=1/2) または通常 file fd を iovec 経由で書き込む。 */
static kinowasm_result_t fd_write(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t iovs = (uint32_t)GETPARAM_INT(1);
	int32_t len = GETPARAM_INT(2);
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(3);

	if(len < 0 || len > WASI_IOVEC_MAX) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	void* handle = NULL;
	int is_stdio = 0;
	FILE* stdio_fp = NULL;
	if(fd == 1) {
		is_stdio = 1;
		stdio_fp = stdout;
	} else if(fd == 2) {
		is_stdio = 1;
		stdio_fp = stderr;
	} else if(fd == 0) {
		/* stdin への書き込みは無効 */
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	} else {
		wasi_fd_t* entry = wasi_fd_get(fd);
		if(entry == NULL || entry->is_preopen || entry->handle == NULL) {
			SETRET_INT(0, __WASI_EBADF);
			return 0;
		}
		if(wasi_vfs->write == NULL) {
			SETRET_INT(0, __WASI_ENOSYS);
			return 0;
		}
		handle = entry->handle;
	}

	uint32_t write_byte = wasi_write_iovs_from_handle(api_call, iovs, len, handle, stdio_fp, is_stdio);
	/* 返却先が線形メモリ範囲外だとゲストは書込バイト数を知れず失敗と誤認して再試行 (重複書込) しうる。
	 * 書込 (副作用) 自体は完了済みだが WASI 仕様どおり EFAULT を返す。 */
	if(kinowasm_write_memory(api_call, ret_ptr, &write_byte, sizeof(write_byte)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_pwrite: offset 指定 write。handle の位置は復元する。 */
static kinowasm_result_t fd_pwrite(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t iovs = (uint32_t)GETPARAM_INT(1);
	int32_t len = GETPARAM_INT(2);
	int64_t offset = api_call->args->data[3].val.num.i64;
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(4);

	if(len < 0 || len > WASI_IOVEC_MAX) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || entry->is_preopen || entry->handle == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	if(wasi_vfs->write == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}

	int64_t saved = wasi_vfs->tell(entry->handle);
	if(saved < 0 || wasi_vfs->seek(entry->handle, offset, SEEK_SET) < 0) {
		SETRET_INT(0, __WASI_EIO);
		return 0;
	}

	uint32_t write_byte = wasi_write_iovs_from_handle(api_call, iovs, len, entry->handle, NULL, 0);
	wasi_vfs->seek(entry->handle, saved, SEEK_SET);
	/* fd_write と同様、返却先不正は EFAULT (書込済みバイト数を失わせない)。 */
	if(kinowasm_write_memory(api_call, ret_ptr, &write_byte, sizeof(write_byte)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* argv / environ の合計バイト数 (各文字列の strlen + 1 の和) を求める共通実装。 */
static uint32_t wasi_strings_buf_size(const char* const* strs, size_t count)
{
	/* #38: uint32_t で累算すると argv/environ 合計が 4GiB 超で wrap し、*_sizes_get が
	 * 過小サイズを報告→*_get が full content を書く不整合になる。uint64_t で累算し
	 * u32 WASM ABI の表現可能上限 (UINT32_MAX) で飽和させる。 */
	uint64_t total = 0;
	for(size_t i = 0; i < count; i++) {
		total += (uint64_t)strlen(strs[i]) + 1u;
		if(total > UINT32_MAX)
			return UINT32_MAX;
	}
	return (uint32_t)total;
}

/* argv / environ を WASM linear memory に展開する共通実装。
 *   ptr_array_addr: 各文字列へのポインタ配列の先頭 WASM アドレス (i32 × count)
 *   buf_addr      : 文字列本体を連結して書き込む WASM アドレス
 *   strs / count  : ホスト側の文字列配列とその要素数
 * ptr 配列には buf 内の絶対 WASM アドレスを書き込む。
 * 返却先が線形メモリ範囲外の場合は __WASI_EFAULT を返す (呼び出し側が errno として返却)。 */
static kinowasm_result_t wasi_write_strings(api_call_t api_call,
	uint32_t ptr_array_addr, uint32_t buf_addr,
	const char* const* strs, size_t count)
{
	uint32_t cur = buf_addr;
	for(size_t i = 0; i < count; i++) {
		uint32_t entry = cur;
		if(kinowasm_write_memory(api_call, ptr_array_addr + (uint32_t)i * 4u, &entry, sizeof(entry)) != RES_SUCCESS)
			return __WASI_EFAULT;
		size_t len = strlen(strs[i]) + 1;
		if(kinowasm_write_memory(api_call, cur, strs[i], len) != RES_SUCCESS)
			return __WASI_EFAULT;
		cur += (uint32_t)len;
	}
	return __WASI_ESUCCESS;
}

static kinowasm_result_t environ_get(api_call_t api_call)
{
	uint32_t envp_addr = (uint32_t)GETPARAM_INT(0);
	uint32_t buf_addr = (uint32_t)GETPARAM_INT(1);
	size_t count = wasi_envp_count();
	if(count > 0) {
		kinowasm_result_t werr = wasi_write_strings(api_call, envp_addr, buf_addr, wasi_envp, count);
		if(werr != __WASI_ESUCCESS) {
			SETRET_INT(0, werr);
			return 0;
		}
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static kinowasm_result_t environ_sizes_get(api_call_t api_call)
{
	uint32_t ret_count = (uint32_t)GETPARAM_INT(0);
	uint32_t ret_buf_size = (uint32_t)GETPARAM_INT(1);
	size_t count_sz = wasi_envp_count();
	uint32_t count = (uint32_t)count_sz;
	uint32_t size = (count_sz > 0) ? wasi_strings_buf_size(wasi_envp, count_sz) : 0u;

	if(kinowasm_write_memory(api_call, ret_count, &count, sizeof(count)) != RES_SUCCESS
		|| kinowasm_write_memory(api_call, ret_buf_size, &size, sizeof(size)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static kinowasm_result_t args_sizes_get(api_call_t api_call)
{
	uint32_t ret_count = (uint32_t)GETPARAM_INT(0);
	uint32_t ret_buf_size = (uint32_t)GETPARAM_INT(1);
	uint32_t count = (uint32_t)wasi_argc;
	uint32_t size = (wasi_argc > 0)
		? wasi_strings_buf_size(wasi_argv, (size_t)wasi_argc)
		: 0u;

	if(kinowasm_write_memory(api_call, ret_count, &count, sizeof(count)) != RES_SUCCESS
		|| kinowasm_write_memory(api_call, ret_buf_size, &size, sizeof(size)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static kinowasm_result_t args_get(api_call_t api_call)
{
	uint32_t argv_addr = (uint32_t)GETPARAM_INT(0);
	uint32_t argv_buf = (uint32_t)GETPARAM_INT(1);
	if(wasi_argc > 0) {
		kinowasm_result_t werr = wasi_write_strings(api_call, argv_addr, argv_buf, wasi_argv, (size_t)wasi_argc);
		if(werr != __WASI_ESUCCESS) {
			SETRET_INT(0, werr);
			return 0;
		}
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static kinowasm_result_t fd_fdstat_get(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t stat_ptr = (uint32_t)GETPARAM_INT(1);

	__wasi_fdstat_t fdstat;
	memset(&fdstat, 0, sizeof(fdstat));

	if(fd >= 0 && fd <= 2) {
		fdstat.fs_filetype = __WASI_FILETYPE_CHARACTER_DEVICE;
		if(fd == 0)
			fdstat.fs_rights_base = __WASI_RIGHT_FD_READ | __WASI_RIGHT_POLL_FD_READWRITE;
		else
			fdstat.fs_rights_base = __WASI_RIGHT_FD_WRITE | __WASI_RIGHT_POLL_FD_READWRITE;
		fdstat.fs_rights_inheriting = fdstat.fs_rights_base;
		if(kinowasm_write_memory(api_call, stat_ptr, &fdstat, sizeof(fdstat)) != RES_SUCCESS) {
			SETRET_INT(0, __WASI_EFAULT);
			return 0;
		}
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	fdstat.fs_filetype = entry->filetype;
	if(entry->is_preopen) {
		fdstat.fs_rights_base = __WASI_RIGHT_PATH_OPEN | __WASI_RIGHT_PATH_FILESTAT_GET
		                      | __WASI_RIGHT_FD_FILESTAT_GET;
	} else {
		fdstat.fs_rights_base = __WASI_RIGHT_FD_READ | __WASI_RIGHT_FD_SEEK
		                      | __WASI_RIGHT_FD_TELL | __WASI_RIGHT_FD_FILESTAT_GET;
	}
	fdstat.fs_rights_inheriting = fdstat.fs_rights_base;
	if(kinowasm_write_memory(api_call, stat_ptr, &fdstat, sizeof(fdstat)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* __wasi_filestat_t (合計 64 byte):
 *   0..7   dev (u64)
 *   8..15  ino (u64)
 *   16     filetype (u8)
 *   17..23 padding
 *   24..31 nlink (u64)
 *   32..39 size (u64)
 *   40..47 atim (u64 ns)
 *   48..55 mtim (u64 ns)
 *   56..63 ctim (u64 ns) */
typedef struct {
	uint64_t dev;
	uint64_t ino;
	uint8_t filetype;
	uint8_t _pad[7];
	uint64_t nlink;
	uint64_t size;
	uint64_t atim;
	uint64_t mtim;
	uint64_t ctim;
} __wasi_filestat_t;

/* host path から filestat を埋める。filetype 引数 0 = VFS の判定に任せる。 */
static kinowasm_result_t wasi_fill_filestat(__wasi_filestat_t* out, const char* host_path, uint8_t filetype_hint)
{
	wasi_vfs_filestat_t fs;
	/* カスタム VFS が filestat を持たない場合の NULL 呼び出しを防ぐ (path_open の検査と対称)。 */
	if(wasi_vfs->filestat == NULL)
		return __WASI_ENOSYS;
	if(wasi_vfs->filestat(host_path, &fs) != 0)
		return __WASI_EBADF;
	memset(out, 0, sizeof(*out));
	out->filetype = (filetype_hint != 0) ? filetype_hint : fs.filetype;
	out->nlink = 1;
	out->size = fs.size;
	out->atim = fs.atim_ns;
	out->mtim = fs.mtim_ns;
	out->ctim = fs.ctim_ns;
	return __WASI_ESUCCESS;
}

static kinowasm_result_t fd_filestat_get(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t out_ptr = (uint32_t)GETPARAM_INT(1);

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	__wasi_filestat_t fs;
	kinowasm_result_t err = wasi_fill_filestat(&fs, entry->host_path, entry->filetype);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}
	if(kinowasm_write_memory(api_call, out_ptr, &fs, sizeof(fs)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* path_filestat_get: dirfd 配下のファイルを stat。
 * 引数: dirfd, dirflags, path_addr, path_len, out_ptr */
static kinowasm_result_t wasi_resolve_path(api_call_t api_call,
	int32_t dirfd, uint32_t path_addr, uint32_t path_len,
	char** out_full);

static kinowasm_result_t path_filestat_get(api_call_t api_call)
{
	int32_t dirfd = GETPARAM_INT(0);
	(void)GETPARAM_INT(1); /* dirflags */
	uint32_t path_addr = (uint32_t)GETPARAM_INT(2);
	uint32_t path_len = (uint32_t)GETPARAM_INT(3);
	uint32_t out_ptr = (uint32_t)GETPARAM_INT(4);

	char* full = NULL;
	kinowasm_result_t r = wasi_resolve_path(api_call, dirfd, path_addr, path_len, &full);
	if(r != __WASI_ESUCCESS) {
		SETRET_INT(0, r);
		return 0;
	}

	__wasi_filestat_t fs;
	/* filetype_hint = 0 で VFS の判定を採用 (DIRECTORY/REGULAR_FILE)。 */
	kinowasm_result_t err = wasi_fill_filestat(&fs, full, 0);
	free(full);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}
	if(kinowasm_write_memory(api_call, out_ptr, &fs, sizeof(fs)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* dirfd + path を読み取り、preopen 配下のフルパスを組み立てる共通実装。
 * path traversal を検査し、安全な場合のみ malloc した文字列を *out_full に
 * 設定して __WASI_ESUCCESS を返す。失敗時は適切な errno を返し *out_full は不変。 */
static kinowasm_result_t wasi_resolve_path(api_call_t api_call,
	int32_t dirfd, uint32_t path_addr, uint32_t path_len,
	char** out_full)
{
	wasi_fd_t* dir_entry = wasi_fd_get(dirfd);
	if(dir_entry == NULL)
		return __WASI_EBADF;
	/* 受理する dirfd: preopen か、path_open(OFLAGS_DIRECTORY) で得た
	 * directory fd。後者は filetype == DIRECTORY かつ host_path 保持。 */
	if(!dir_entry->is_preopen && dir_entry->filetype != __WASI_FILETYPE_DIRECTORY)
		return __WASI_EBADF;
	if(path_len > WASI_PATH_MAX)
		return __WASI_EOVERFLOW;
	char path[WASI_PATH_MAX + 1];
	/* セキュリティ: 読み取り失敗時は未初期化 path を使わず EFAULT を返す (read_memory は all-or-nothing)。 */
	if(path_len > 0 && kinowasm_read_memory(api_call, path_addr, path, path_len) != RES_SUCCESS)
		return __WASI_EFAULT;
	path[path_len] = '\0';

	if(!wasi_path_is_safe(path, path_len))
		return __WASI_EINVAL;

	const char* base = dir_entry->is_preopen
		? wasi_preopens[dir_entry->preopen_idx].host_path
		: dir_entry->host_path;
	if(base == NULL)
		return __WASI_EBADF;
	size_t base_len = strlen(base);
	/* base_len + path_len + 2 の overflow ガード (path_open と同じ方針) */
	if(base_len > SIZE_MAX - WASI_PATH_MAX - 2)
		return __WASI_EOVERFLOW;
	size_t total = base_len + 1 + path_len + 1;
	char* full = (char*)malloc(total);
	if(full == NULL)
		return __WASI_ENOMEM;
	memcpy(full, base, base_len);
	full[base_len] = '/';
	memcpy(full + base_len + 1, path, path_len);
	full[base_len + 1 + path_len] = '\0';
	/* #5: symlink/junction を解決した実パスが base subtree 内に収まることを検証。 */
	if(!wasi_path_contained(base, full)) {
		free(full);
		return __WASI_EACCES;
	}
	*out_full = full;
	return __WASI_ESUCCESS;
}

/* path_create_directory / path_remove_directory / path_unlink_file 共通:
 * dirfd+path を解決し、単一引数の vfs 操作 (mkdir/rmdir/unlink) を適用して errno を返す。 */
static kinowasm_result_t wasi_simple_path_op(api_call_t api_call, int (*vfs_fn)(const char*))
{
	int32_t dirfd = GETPARAM_INT(0);
	uint32_t path_addr = (uint32_t)GETPARAM_INT(1);
	uint32_t path_len = (uint32_t)GETPARAM_INT(2);

	if(vfs_fn == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}
	char* full = NULL;
	kinowasm_result_t err = wasi_resolve_path(api_call, dirfd, path_addr, path_len, &full);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}
	errno = 0;
	int rc = vfs_fn(full);
	uint32_t e = (rc == 0) ? __WASI_ESUCCESS : wasi_errno_from_host(errno);
	free(full);
	SETRET_INT(0, e);
	return 0;
}

static kinowasm_result_t path_create_directory(api_call_t api_call)
{
	return wasi_simple_path_op(api_call, wasi_vfs->mkdir);
}
static kinowasm_result_t path_remove_directory(api_call_t api_call)
{
	return wasi_simple_path_op(api_call, wasi_vfs->rmdir);
}
static kinowasm_result_t path_unlink_file(api_call_t api_call)
{
	return wasi_simple_path_op(api_call, wasi_vfs->unlink);
}

/* path_rename: 引数 (old_fd, old_path_addr, old_path_len, new_fd, new_path_addr, new_path_len)
 * path_rename / path_link 共通: 2 つの dirfd+path を解決し、2 引数 vfs 操作 (rename/link) を
 * 適用する。両 path とも安全解決し、失敗時は確保済みを解放して errno を返す。 */
static kinowasm_result_t wasi_two_path_op(api_call_t api_call,
	int32_t old_fd, uint32_t old_addr, uint32_t old_len,
	int32_t new_fd, uint32_t new_addr, uint32_t new_len,
	int (*vfs_fn)(const char*, const char*))
{
	if(vfs_fn == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}
	char* old_full = NULL;
	char* new_full = NULL;
	kinowasm_result_t err = wasi_resolve_path(api_call, old_fd, old_addr, old_len, &old_full);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}
	err = wasi_resolve_path(api_call, new_fd, new_addr, new_len, &new_full);
	if(err != __WASI_ESUCCESS) {
		free(old_full);
		SETRET_INT(0, err);
		return 0;
	}
	errno = 0;
	int rc = vfs_fn(old_full, new_full);
	uint32_t e = (rc == 0) ? __WASI_ESUCCESS : wasi_errno_from_host(errno);
	free(old_full);
	free(new_full);
	SETRET_INT(0, e);
	return 0;
}

static kinowasm_result_t path_rename(api_call_t api_call)
{
	return wasi_two_path_op(api_call,
		GETPARAM_INT(0), (uint32_t)GETPARAM_INT(1), (uint32_t)GETPARAM_INT(2),
		GETPARAM_INT(3), (uint32_t)GETPARAM_INT(4), (uint32_t)GETPARAM_INT(5),
		wasi_vfs->rename);
}

/* fd_readdir: directory fd を cookie ベースで列挙する。
 * 引数: fd, buf_addr, buf_len, cookie (i64), bufused_ptr
 * dirent layout (24 byte):
 *   0..7   d_next  (u64) 次回 cookie として渡せる値 = 直前 entry の通し番号 +1
 *   8..15  d_ino   (u64) 0 (inode 不明)
 *   16..19 d_namlen (u32)
 *   20     d_type  (u8)
 *   21..23 padding
 *   24..   name bytes (NUL なし)
 * バッファ末尾に達したら途中で打ち切るが、written 値は実際に書いた byte 数を
 * buf_len で頭打ちにする。caller は (buf_used == buf_len) なら継続要求と判定する。 */
static kinowasm_result_t fd_readdir(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t buf_addr = (uint32_t)GETPARAM_INT(1);
	uint32_t buf_len = (uint32_t)GETPARAM_INT(2);
	int64_t cookie_s = api_call->args->data[3].val.num.i64;
	uint32_t bufused_ptr = (uint32_t)GETPARAM_INT(4);

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || entry->filetype != __WASI_FILETYPE_DIRECTORY) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	if(wasi_vfs->opendir == NULL || wasi_vfs->readdir_next == NULL || wasi_vfs->closedir == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}

	const char* host_path = entry->is_preopen
		? wasi_preopens[entry->preopen_idx].host_path
		: entry->host_path;
	void* dh = wasi_vfs->opendir(host_path);
	if(dh == NULL) {
		/* opendir 失敗は EBADF を優先する。bufused=0 の書込失敗は結果に影響しないため無視する。 */
		uint32_t bu = 0;
		kinowasm_write_memory(api_call, bufused_ptr, &bu, sizeof(bu));
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}

	uint64_t cookie = (cookie_s < 0) ? 0 : (uint64_t)cookie_s;
	uint64_t cur_cookie = 0;
	wasi_vfs_dirent_t de;
	/* cookie 件分スキップ */
	while(cur_cookie < cookie) {
		int r = wasi_vfs->readdir_next(dh, &de);
		if(r <= 0) {
			wasi_vfs->closedir(dh);
			uint32_t bu = 0;
			if(kinowasm_write_memory(api_call, bufused_ptr, &bu, sizeof(bu)) != RES_SUCCESS) {
				SETRET_INT(0, __WASI_EFAULT);
				return 0;
			}
			SETRET_INT(0, __WASI_ESUCCESS);
			return 0;
		}
		cur_cookie++;
	}

	uint32_t written = 0;
	for(;;) {
		int r = wasi_vfs->readdir_next(dh, &de);
		if(r <= 0)
			break;
		cur_cookie++;

		uint8_t header[24];
		memset(header, 0, sizeof(header));
		uint64_t d_next = cur_cookie;
		uint64_t d_ino = 0;
		uint32_t d_namlen = (uint32_t)strlen(de.name);
		memcpy(header + 0, &d_next, 8);
		memcpy(header + 8, &d_ino, 8);
		memcpy(header + 16, &d_namlen, 4);
		header[20] = de.filetype;

		uint32_t hdr_left = (written < buf_len) ? (buf_len - written) : 0;
		uint32_t hdr_copy = (24u < hdr_left) ? 24u : hdr_left;
		if(hdr_copy > 0) {
			/* 書込失敗時に written を進めて bufused に計上すると過大報告になるため EFAULT。 */
			if(kinowasm_write_memory(api_call, buf_addr + written, header, hdr_copy) != RES_SUCCESS) {
				wasi_vfs->closedir(dh);
				SETRET_INT(0, __WASI_EFAULT);
				return 0;
			}
		}
		written += 24u;
		if(written >= buf_len) {
			written = buf_len;
			break;
		}

		uint32_t name_left = buf_len - written;
		uint32_t name_copy = (d_namlen < name_left) ? d_namlen : name_left;
		if(name_copy > 0) {
			if(kinowasm_write_memory(api_call, buf_addr + written, de.name, name_copy) != RES_SUCCESS) {
				wasi_vfs->closedir(dh);
				SETRET_INT(0, __WASI_EFAULT);
				return 0;
			}
		}
		written += d_namlen;
		if(written >= buf_len) {
			written = buf_len;
			break;
		}
	}

	wasi_vfs->closedir(dh);
	if(kinowasm_write_memory(api_call, bufused_ptr, &written, sizeof(written)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* path_link: 既存ファイル (old_fd 配下) に対する hard link を new_fd 配下に作成。
 * 引数: old_fd, old_flags (LOOKUP_SYMLINK_FOLLOW), old_path_addr, old_path_len,
 *       new_fd, new_path_addr, new_path_len */
static kinowasm_result_t path_link(api_call_t api_call)
{
	/* param 1 は old_flags (lookupflags、未使用) */
	return wasi_two_path_op(api_call,
		GETPARAM_INT(0), (uint32_t)GETPARAM_INT(2), (uint32_t)GETPARAM_INT(3),
		GETPARAM_INT(4), (uint32_t)GETPARAM_INT(5), (uint32_t)GETPARAM_INT(6),
		wasi_vfs->link);
}

/* path_symlink: target を指す symbolic link を fd 配下の new_path に作成。
 * セキュリティ: target は link 位置を基準に解決されるため、絶対パスや ".." を含む
 * target を許すと preopen 外を指す symlink を作成でき、後続の path_open/filestat が
 * それを follow して sandbox を逸脱できる (本ランタイムは open 時の安全解決を行わない)。
 * そのため new_path 同様 wasi_path_is_safe で target を検査し、逸脱する target は拒否する。
 * 引数: old_path_addr, old_path_len (target), fd, new_path_addr, new_path_len (link name) */
static kinowasm_result_t path_symlink(api_call_t api_call)
{
	uint32_t target_addr = (uint32_t)GETPARAM_INT(0);
	uint32_t target_len = (uint32_t)GETPARAM_INT(1);
	int32_t fd = GETPARAM_INT(2);
	uint32_t new_path_addr = (uint32_t)GETPARAM_INT(3);
	uint32_t new_path_len = (uint32_t)GETPARAM_INT(4);

	if(wasi_vfs->symlink == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}
	if(target_len > WASI_PATH_MAX) {
		SETRET_INT(0, __WASI_EOVERFLOW);
		return 0;
	}
	char target[WASI_PATH_MAX + 1];
	/* セキュリティ: 読み取り失敗時は未初期化 target を使わず EFAULT を返す (read_memory は all-or-nothing)。 */
	if(target_len > 0 && kinowasm_read_memory(api_call, target_addr, target, target_len) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	target[target_len] = '\0';

	/* セキュリティ: sandbox 外を指す symlink target (絶対パス / "..") は拒否する。 */
	if(!wasi_path_is_safe(target, target_len)) {
		SETRET_INT(0, __WASI_EACCES);
		return 0;
	}

	char* new_full = NULL;
	kinowasm_result_t err = wasi_resolve_path(api_call, fd, new_path_addr, new_path_len, &new_full);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}
	errno = 0;
	int rc = wasi_vfs->symlink(target, new_full);
	uint32_t e = (rc == 0) ? __WASI_ESUCCESS : wasi_errno_from_host(errno);
	free(new_full);
	SETRET_INT(0, e);
	return 0;
}

/* path_readlink: fd 配下の symbolic link を読み取り、target 文字列を buf に書く。
 * 引数: fd, path_addr, path_len, buf_addr, buf_len, bufused_ptr */
static kinowasm_result_t path_readlink(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t path_addr = (uint32_t)GETPARAM_INT(1);
	uint32_t path_len = (uint32_t)GETPARAM_INT(2);
	uint32_t buf_addr = (uint32_t)GETPARAM_INT(3);
	uint32_t buf_len = (uint32_t)GETPARAM_INT(4);
	uint32_t bufused_ptr = (uint32_t)GETPARAM_INT(5);

	if(wasi_vfs->readlink == NULL) {
		SETRET_INT(0, __WASI_ENOSYS);
		return 0;
	}
	char* full = NULL;
	kinowasm_result_t err = wasi_resolve_path(api_call, fd, path_addr, path_len, &full);
	if(err != __WASI_ESUCCESS) {
		SETRET_INT(0, err);
		return 0;
	}

	if(buf_len == 0) {
		free(full);
		uint32_t zero = 0;
		if(kinowasm_write_memory(api_call, bufused_ptr, &zero, sizeof(zero)) != RES_SUCCESS) {
			SETRET_INT(0, __WASI_EFAULT);
			return 0;
		}
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}

	/* #17: ゲスト提供 buf_len を無上限で malloc しない (単一呼び出しで ~4GB 確保を強制される)。
	 * readlink target が WASI_STAGE_MAX を超えることは現実に無く、超過分は truncate 扱いで安全。 */
	uint32_t cap = buf_len < WASI_STAGE_MAX ? buf_len : WASI_STAGE_MAX;
	char* tmp = (char*)malloc(cap);
	if(tmp == NULL) {
		free(full);
		SETRET_INT(0, __WASI_ENOMEM);
		return 0;
	}
	int64_t got = wasi_vfs->readlink(full, tmp, cap);
	free(full);
	if(got == -2) {
		/* reparse point / symlink でない */
		free(tmp);
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}
	if(got < 0) {
		free(tmp);
		SETRET_INT(0, __WASI_EIO);
		return 0;
	}
	uint32_t got_u = (uint32_t)got;
	if(got_u > cap)
		got_u = cap;
	/* 本体書込が失敗した場合に bufused に got_u を報告すると、書けていないバイトを
	 * 書けたと誤報告してゲストが未初期化バッファを読む。失敗時は EFAULT を返す。 */
	if(got_u > 0) {
		if(kinowasm_write_memory(api_call, buf_addr, tmp, got_u) != RES_SUCCESS) {
			free(tmp);
			SETRET_INT(0, __WASI_EFAULT);
			return 0;
		}
	}
	free(tmp);
	if(kinowasm_write_memory(api_call, bufused_ptr, &got_u, sizeof(got_u)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static kinowasm_result_t proc_exit(api_call_t api_call)
{
	int32_t code = GETPARAM_INT(0);
	wasi_exit_called = 1;
	wasi_exit_code   = code;
	/* trap で WASM 実行を即時終了。ホスト側は wasi_get_exit_status で
	 * 「これは proc_exit による正常終了」と認識して、code を終了コードに使う。 */
	return 1;
}

/* -------------------------------------------------------------------------
 * 未実装 WASI Preview1 関数の ENOSYS stub。wasi-libc 系 toolchain は
 * 利用しない関数も含めて全部 import するため、binding を欠くと
 * モジュール load 時に ERR_UNKNOWN_IMPORT_SYMBOL になる。利用時の
 * 振る舞いは spec 通り __WASI_ENOSYS を返すだけで実害はない。
 *
 * 例外: sched_yield は no-op 成功を返す (シングルスレッド runtime では
 * yield する相手がいないため)。
 * ------------------------------------------------------------------------- */

static kinowasm_result_t wasi_stub_enosys(api_call_t api_call)
{
	(void)api_call;
	SETRET_INT(0, __WASI_ENOSYS);
	return 0;
}

static kinowasm_result_t wasi_sched_yield_stub(api_call_t api_call)
{
	(void)api_call;
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* sock_shutdown(fd, how): KinoRuntime はソケット未対応のため、
 * fd が socket でなければ EBADF (またはOS仕様により ENOTSOCK)、
 * socket でも実体無いので最終的に ENOSYS。
 * wasi-testsuite が EBADF / ENOTSOCK を期待するため明示的に分岐する。
 */
static kinowasm_result_t wasi_sock_shutdown_stub(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	(void)GETPARAM_INT(1);
	if(fd < 0) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	if(fd >= 0 && fd <= 2) {
		/* ENOTSOCK は WASI でコード 57。define 追加せず数値で。 */
		SETRET_INT(0, 57);
		return 0;
	}
	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	/* 通常 file/dir に対する shutdown は ENOTSOCK。 */
	SETRET_INT(0, 57);
	return 0;
}

/* fd_renumber(src, dst): dst を閉じてから src を dst の slot に移し、src を解放。
 * 0/1/2 (stdio) や preopen は WASI 仕様上 renumber 不可とまではされないが、
 * KinoRuntime では preopen を移すと preopen テーブルとの整合が崩れるため
 * EBADF を返す。 */
static kinowasm_result_t fd_renumber_impl(api_call_t api_call)
{
	int32_t src = GETPARAM_INT(0);
	int32_t dst = GETPARAM_INT(1);

	wasi_fd_t* src_e = wasi_fd_get(src);
	if(src_e == NULL || src_e->is_preopen) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	/* dst < WASI_FD_BASE は減算前に弾く (dst=INT_MIN だと dst - WASI_FD_BASE が
	 * signed overflow で UB。wasi_fd_get の検査順と対称)。 */
	if(dst < WASI_FD_BASE) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	int dst_idx = dst - WASI_FD_BASE;
	if(dst_idx >= WASI_FD_MAX) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	/* dst が preopen 領域 (idx 0..preopen_count-1) を指す場合、その slot は
	 * sandbox の root directory なので上書き不可。capability sandbox 維持のため reject。 */
	if(dst_idx < wasi_preopen_count) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	int src_idx = src - WASI_FD_BASE;
	if(src_idx == dst_idx) {
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}
	wasi_fd_t* dst_e = &wasi_fds[dst_idx];
	if(dst_e->is_used && !dst_e->is_preopen) {
		if(dst_e->handle != NULL)
			wasi_vfs->close(dst_e->handle);
		free(dst_e->host_path);
	}
	*dst_e = *src_e;
	memset(src_e, 0, sizeof(*src_e));
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* REALTIME (wall clock since Unix epoch) をナノ秒で返す。
 * Windows: FILETIME (100ns 単位、1601-01-01 起算) を 1970-01-01 起算 ns に変換。
 * 1601 → 1970 のオフセット: 11,644,473,600 秒 = 116,444,736,000,000,000 (100ns 単位)。 */
static uint64_t wasi_realtime_ns(void)
{
#if defined(_WIN32)
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t intervals = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
	intervals -= 116444736000000000ULL;
	return intervals * 100ULL;
#else
	return (uint64_t)0;
#endif
}

/* MONOTONIC (起動時 0 起算) をナノ秒精度で返す。get_running_time() は float
 * で精度が落ちるため、ここでは直接 QPC を呼んで uint64_t 精度を得る。 */
static uint64_t wasi_monotonic_ns(void)
{
#if defined(_WIN32)
	static LARGE_INTEGER mono_freq = { 0 };
	static LARGE_INTEGER mono_start = { 0 };
	if(mono_freq.QuadPart == 0) {
		QueryPerformanceFrequency(&mono_freq);
		QueryPerformanceCounter(&mono_start);
	}
	LARGE_INTEGER cur;
	QueryPerformanceCounter(&cur);
	uint64_t ticks = (uint64_t)(cur.QuadPart - mono_start.QuadPart);
	/* ticks * 1e9 / freq でオーバーフロー回避のため、秒と剰余に分解。 */
	uint64_t freq_q = (uint64_t)mono_freq.QuadPart;
	uint64_t sec = ticks / freq_q;
	uint64_t rem = ticks % freq_q;
	return sec * 1000000000ULL + (rem * 1000000000ULL) / freq_q;
#else
	return (uint64_t)0;
#endif
}

static kinowasm_result_t clock_time_get(api_call_t api_call)
{
	int32_t id = GETPARAM_INT(0);
	/* precision (引数 1, i64) は実装ヒントであり無視する。 */
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(2);

	uint64_t time_ns = 0;
	switch(id) {
	case __WASI_CLOCK_REALTIME:
		time_ns = wasi_realtime_ns();
		break;
	case __WASI_CLOCK_MONOTONIC:
	case __WASI_CLOCK_PROCESS_CPUTIME_ID:
	case __WASI_CLOCK_THREAD_CPUTIME_ID:
		time_ns = wasi_monotonic_ns();
		break;
	default:
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	if(kinowasm_write_memory(api_call, ret_ptr, &time_ns, sizeof(time_ns)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* clock_res_get: clock の解像度をナノ秒で返す。
 * REALTIME / MONOTONIC ともに QPC 由来 (~100ns) として 1ns を返しておけば
 * 大半の Rust/Go プログラムは満足する (実際の精度より細かい値でも問題ない)。 */
static kinowasm_result_t clock_res_get(api_call_t api_call)
{
	int32_t id = GETPARAM_INT(0);
	uint32_t ret_ptr = (uint32_t)GETPARAM_INT(1);

	if(id < __WASI_CLOCK_REALTIME || id > __WASI_CLOCK_THREAD_CPUTIME_ID) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}

	uint64_t resolution_ns = 1ULL;
	if(kinowasm_write_memory(api_call, ret_ptr, &resolution_ns, sizeof(resolution_ns)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* random_get: 高品質乱数で buf を埋める。
 * Windows では rand_s (RtlGenRandom 経由) を使用。
 * 非 Windows の fallback は低品質 (rand) のため将来 getrandom 等に差し替える。 */
static kinowasm_result_t random_get(api_call_t api_call)
{
	uint32_t buf_addr = (uint32_t)GETPARAM_INT(0);
	uint32_t buf_len = (uint32_t)GETPARAM_INT(1);

	if(buf_len == 0) {
		SETRET_INT(0, __WASI_ESUCCESS);
		return 0;
	}

	/* #17: staging を WASI_STAGE_MAX で頭打ちにし、buf 全体をチャンク分割して埋める。 */
	uint32_t cap = buf_len < WASI_STAGE_MAX ? buf_len : WASI_STAGE_MAX;
	uint8_t* tmp = (uint8_t*)kinowasm_mem_malloc(cap);
	if(tmp == NULL) {
		SETRET_INT(0, __WASI_ENOMEM);
		return 0;
	}

	uint32_t done = 0;
	while(done < buf_len) {
		uint32_t chunk = (buf_len - done) < cap ? (buf_len - done) : cap;
#if defined(_WIN32)
		uint32_t i = 0;
		while(i < chunk) {
			unsigned int v;
			if(rand_s(&v) != 0) {
				kinowasm_mem_free(tmp);
				SETRET_INT(0, __WASI_EIO);
				return 0;
			}
			uint32_t n = (chunk - i) < sizeof(v) ? (chunk - i) : (uint32_t)sizeof(v);
			memcpy(tmp + i, &v, n);
			i += n;
		}
#else
		for(uint32_t i = 0; i < chunk; i++)
			tmp[i] = (uint8_t)(rand() & 0xFF);
#endif
		/* 返却先不正を無視すると未初期化のゲストバッファを乱数と誤用させるため EFAULT を返す。 */
		if(kinowasm_write_memory(api_call, buf_addr + done, tmp, chunk) != RES_SUCCESS) {
			kinowasm_mem_free(tmp);
			SETRET_INT(0, __WASI_EFAULT);
			return 0;
		}
		done += chunk;
	}

	kinowasm_mem_free(tmp);
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_prestat_get: preopened directory の存在と name 長を返す。
 * preopen が無い fd に対しては EBADF を返し、WASI runtime は列挙を打ち切る。
 * __wasi_prestat_t レイアウト: { u8 tag; u8 _pad[3]; u32 pr_name_len; } (合計 8 byte) */
static kinowasm_result_t fd_prestat_get(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t prestat_ptr = (uint32_t)GETPARAM_INT(1);

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || !entry->is_preopen) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}

	uint8_t prestat[8];
	memset(prestat, 0, sizeof(prestat));
	prestat[0] = __WASI_PREOPENTYPE_DIR;
	uint32_t name_len = (uint32_t)strlen(wasi_preopens[entry->preopen_idx].wasi_name);
	memcpy(&prestat[4], &name_len, sizeof(name_len));
	if(kinowasm_write_memory(api_call, prestat_ptr, prestat, sizeof(prestat)) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

/* fd_prestat_dir_name: preopen の WASI 公開名を path_addr に書き込む。
 * path_len は呼び出し側が確保したバッファ長で、name_len と一致しなければ EINVAL。 */
static kinowasm_result_t fd_prestat_dir_name(api_call_t api_call)
{
	int32_t fd = GETPARAM_INT(0);
	uint32_t path_addr = (uint32_t)GETPARAM_INT(1);
	uint32_t path_len = (uint32_t)GETPARAM_INT(2);

	wasi_fd_t* entry = wasi_fd_get(fd);
	if(entry == NULL || !entry->is_preopen) {
		SETRET_INT(0, __WASI_EBADF);
		return 0;
	}
	const char* name = wasi_preopens[entry->preopen_idx].wasi_name;
	uint32_t name_len = (uint32_t)strlen(name);
	if(path_len < name_len) {
		SETRET_INT(0, __WASI_EINVAL);
		return 0;
	}
	if(kinowasm_write_memory(api_call, path_addr, name, name_len) != RES_SUCCESS) {
		SETRET_INT(0, __WASI_EFAULT);
		return 0;
	}
	SETRET_INT(0, __WASI_ESUCCESS);
	return 0;
}

static modulestore_t* find_modulestore(int32_t module_id)
{
	if(module_id < 0 || (size_t)module_id >= module_store_len)
		return NULL;

	if(module_stores[module_id].is_used == 0)
		return NULL;

	return &module_stores[module_id];
}

static modulestore_t* assign_modulestore(int32_t* module_id)
{
	for(size_t i = 0; i < module_store_len; i++) {
		if(module_stores[i].is_used == 0) {
			memset(&module_stores[i], 0, sizeof(module_stores[i]));
			module_stores[i].is_used = 1;
			*module_id = (int32_t)i;
			return &module_stores[i];
		}
	}

	if(module_store_len == module_store_capacity) {
		size_t new_capacity = module_store_capacity == 0 ? 4 : module_store_capacity * 2;
		modulestore_t* new_stores = (modulestore_t*)realloc(module_stores, sizeof(modulestore_t) * new_capacity);
		if(new_stores == NULL)
			return NULL;

		memset(new_stores + module_store_capacity, 0, sizeof(modulestore_t) * (new_capacity - module_store_capacity));
		module_stores = new_stores;
		module_store_capacity = new_capacity;
	}

	memset(&module_stores[module_store_len], 0, sizeof(module_stores[module_store_len]));
	module_stores[module_store_len].is_used = 1;
	*module_id = (int32_t)module_store_len;
	return &module_stores[module_store_len++];
}

static int grow_pushinfo(modulestore_t* module_store)
{
	if(module_store->push_info_len < module_store->push_info_capacity)
		return 1;

	size_t new_capacity = module_store->push_info_capacity == 0 ? 4 : module_store->push_info_capacity * 2;
	modulepushinfo_t* new_push_infos = (modulepushinfo_t*)realloc(module_store->push_infos, sizeof(modulepushinfo_t) * new_capacity);
	if(new_push_infos == NULL)
		return 0;

	memset(new_push_infos + module_store->push_info_capacity, 0, sizeof(modulepushinfo_t) * (new_capacity - module_store->push_info_capacity));
	module_store->push_infos = new_push_infos;
	module_store->push_info_capacity = new_capacity;
	return 1;
}

/* flat 線形メモリ解放 (kw_core_bridge.c)。MSVC C4210 回避のため file scope で宣言する。 */
extern void kw_core_mem_release(uint8_t* base);
extern void kw_core_free_instance(store_t* S, moduleinst_t* inst);   /* pop 時に push で構築した core インスタンスを解放 */
extern void kw_free_moduleinst(moduleinst_t* mi);   /* moduleinst の全所有リソース (メタデータ/exports/addrs) を解放 */

static void free_mem_instance(memoryinstance_t* mem_inst)
{
	/* flat 線形メモリ (kw_alloc_mem が reserve した OS 仮想メモリ) を解放する。
	 * 旧多段ページテーブル (table2) は廃止し store/core で flat バッファを共有する。 */
	if(mem_inst->base != NULL) {
		kw_core_mem_release(mem_inst->base);
		mem_inst->base = NULL;
	}
	mem_inst->num_pages = 0;
}

static kinowasm_result_t rebuild_extra_func_type(store_t* store)
{
	_try{
		kinowasm_extrafuncs_t * extra_func_table = kinowasm_settings.extra_func_table;
		kinowasm_mem_set_info(store->storememory);

		if(store->extra_func_type.item_size != 0) {
			kinowasm_array_term_from(store->extra_func_type);
		}

		if(extra_func_table == NULL) {
			kinowasm_array_init_from(store->extra_func_type);
			_throw(RES_SUCCESS);
		}

		_throwiferr(kinowasm_array_new_from(store->extra_func_type, extra_func_table->len));
		memset(store->extra_func_type.data, 0, sizeof(functiontype_t*) * store->extra_func_type.len);

		kinowasm_array_foreach(moduletable, moduletable_t, store->moduletable) {
			kinowasm_array_foreach(import, import_t, moduletable->module->origin_module.imports) {
				if(import->d.kind != IMPORTDESC_FUNC)
					continue;

				for(uint32_t i = 0; i < extra_func_table->len; i++) {
					if(COMPARE_STRING(import->module.data, kinowasm_array_at((*extra_func_table), i).module) &&
						COMPARE_STRING(import->name.data, kinowasm_array_at((*extra_func_table), i).name)) {
						if(kinowasm_array_at(store->extra_func_type, i) == NULL)
							kinowasm_array_at(store->extra_func_type, i) = &kinowasm_array_at(moduletable->module->origin_module.types, import->d.functypeidx);
						break;
					}
				}
			}
		}
	}
	_catch:
	return _result;
}

static kinowasm_result_t rollback_modulepush(modulestore_t* module_store, const modulepushinfo_t* push_info)
{
	_try{
		store_t * store = (store_t*)module_store->S;
		_throwif(ERR_NOTMODULEINIT, store == NULL);

		kinowasm_mem_set_info(store->storememory);
		kinowasm_reset_store(module_store->S);
		store->state_flags = 0;

		for(size_t i = push_info->datas_len; i < store->datas.len; i++) {
			kinowasm_array_term_from(store->datas.data[i].data);
		}

		for(size_t i = push_info->elements_len; i < store->elements.len; i++) {
			kinowasm_array_term_from(store->elements.data[i].elem);
		}

		for(size_t i = push_info->tables_len; i < store->tables.len; i++) {
			kinowasm_array_term_from(store->tables.data[i].elem);
		}

		for(size_t i = push_info->memorys_len; i < store->memorys.len; i++) {
			free_mem_instance(&store->memorys.data[i]);
		}

		for(size_t i = push_info->moduletable_len; i < store->moduletable.len; i++) {
			moduletable_t* moduletable = &store->moduletable.data[i];
			moduleinst_t* module = moduletable->module;
			if(module != NULL) {
				kw_core_free_instance(store, module);   /* push で構築した core インスタンスを先に解放 (storememory 蓄積防止) */
				/* 旧手書き解放は origin_module メタデータ (types/imports/tags)・export 名
				 * 文字列・tagaddrs を取りこぼし、push/pop の繰り返しで storememory に蓄積
				 * していた (実測: 48MB アリーナが約 32000 回の push/pop で枯渇)。teardown と
				 * 同じ kw_free_moduleinst に一本化する。 */
				kw_free_moduleinst(module);
			}

			kinowasm_array_term_from(moduletable->name);
		}

		store->funcs.len = push_info->funcs_len;
		store->tables.len = push_info->tables_len;
		store->memorys.len = push_info->memorys_len;
		store->globals.len = push_info->globals_len;
		store->elements.len = push_info->elements_len;
		store->datas.len = push_info->datas_len;
		store->moduletable.len = push_info->moduletable_len;

		_throwiferr(rebuild_extra_func_type(store));
	}
	_catch:
	return _result;
}

#if 0
static inline void pool_unlink(memorypool_t* p)
{
	memorypool_t* prev = p->prev;
	memorypool_t* next = p->next;
	if(prev != NULL)
		prev->next = p->next;
	if(next != NULL)
		next->prev = p->prev;

	p->next = NULL;
	p->prev = NULL;
}

static inline void pool_next_link(memorypool_t* target, memorypool_t* p)
{
	p->next = target->next;
	target->next = p;
	p->prev = target;
	if(p->next != NULL)
		p->next->prev = p;
}

static memorytable_t* find_memorystore(kinowasm_handle_t S)
{
	memorytable_t* table = NULL;
	if(memory_table.len == 0) {
		kinowasm_array_init_from(memory_table);
	}
	else {
		for(uint32_t i = 0; i < memory_table.len; i++)
		{
			if(kinowasm_array_at(memory_table, i).S == S) {
				table = &kinowasm_array_at(memory_table, i);
				break;
			}
		}
	}
	if(table == NULL) {
		size_t new_object_count = memory_table.len;
		if(kinowasm_array_grow_from(memory_table, 1) == 0) {
			kinowasm_array_at(memory_table, new_object_count).S = S;
			kinowasm_array_at(memory_table, new_object_count).unuse_memory = (memorypool_t){ NULL, NULL, 0, 0 };
			kinowasm_array_at(memory_table, new_object_count).use_memory = (memorypool_t){ NULL, NULL, 0, 0 };
			table = &kinowasm_array_at(memory_table, new_object_count);
			memory_table.len++;
		}
	}
	return table;
}

static uint32_t assign_memory(api_call_t api_call, uint32_t size)
{
	memorytable_t* table = find_memorystore(api_call->current_store);
	memorypool_t* p = &table->unuse_memory;
	while(p != NULL) {
		if(size == p->memory_size) {
			if(size != 0) {
				/* Unlink */
				pool_unlink(p);
				/* Add Use memory */
				pool_next_link(&table->use_memory, p);
			}
			return p->memory_addr;
		}
		p = p->next;
	}
	p = (memorypool_t*)kinowasm_mem_malloc(sizeof(memorypool_t));
	if(p != NULL) {
		p->prev = NULL;
		p->next = NULL;
		p->memory_size = size;
		p->memory_addr = memory_count;
		memory_count += size;
		/* Add Use memory */
		pool_next_link(&table->use_memory, p);
		return p->memory_addr;
	}
	return 0;
}

static void free_memory(kinowasm_handle_t S, uint32_t addr)
{
	memorytable_t* table = find_memorystore(S);
	memorypool_t* p = &table->use_memory;
	while(p != NULL) {
		if(addr == p->memory_addr) {
			if(p->memory_addr != 0) {
				/* Unlink */
				pool_unlink(p);
				/* Add Use memory */
				pool_next_link(&table->unuse_memory, p);
			}
			return;
		}
		p = p->next;
	}
}
#endif

kinowasm_result_t emscripten_notify_memory_growth(api_call_t api_call)
{
	int32_t memory_index = GETPARAM_INT(0);
	printf("Grow memory %d.\n", memory_index);
	return 0;
}

kinowasm_result_t segfault(api_call_t api_call)
{
	printf("Segmentation Fault.\n");
	return ERR_SEGMENTATION_FAULT;
}

kinowasm_result_t alignfault(api_call_t api_call)
{
	printf("Alignment Fault.\n");
	return ERR_ALIGNMENT_FAULT;
}

static kinowasm_result_t test1(api_call_t api_call)
{
	printf("%d\n", GETPARAM_INT(0));
	return 0;
}

static kinowasm_result_t cmdpow(api_call_t api_call)
{
	int32_t n1 = GETPARAM_INT(0);
	int32_t n2 = GETPARAM_INT(1);
	int32_t r = 0;
	r = 1;
	for(int i = 0; i < n2; i++) {
		r *= n1;
	}
	SETRET_INT(0, r);
	return 0;
}

static kinowasm_result_t console(api_call_t api_call)
{
	uint32_t t = (uint32_t)GETPARAM_INT(0);
	/* NUL までの長さを数える。範囲外 read は #4 で no-op になり c が変化しないため、
	 * 毎回 0 初期化して範囲外を終端扱いにする (無限ループ回避)。上限 64KB。 */
	uint32_t count;
	char c = 0;
	for(count = 0; count < 0x10000; count++) {
		c = 0;
		kinowasm_read_memory(api_call, t + count, &c, sizeof(c));
		if(c == '\0')
			break;
	}
	/* +1 で NUL 終端分を確保。malloc 失敗時はコンソール出力 best-effort のため no-op。 */
	char* str = (char*)kinowasm_mem_malloc(count + 1);
	if(str == NULL)
		return 0;
	kinowasm_read_memory(api_call, t, str, count);
	str[count] = '\0';
	printf("%s\n", str);
	kinowasm_mem_free(str);
	return 0;
}

static kinowasm_result_t get_current_time(api_call_t api_call)
{
	SETRET_FLOAT(0, get_running_time());
	return 0;
}

static kinowasm_result_t wait(api_call_t api_call)
{
	float wait_time = GETPARAM_FLOAT(0);
	system_wait(wait_time);
	return 0;
}

static kinowasm_result_t tz_set(api_call_t api_call)
{
	/* emscripten _tzset_js: 現状はノーオプ。将来 TZ 環境変数を反映する場合に
	 * timezone/daylight/std_name/dst_name を WASM 側へ書き戻す。 */
	(void)api_call;
	return 0;
}

int32_t create_wasm_module(uint32_t module_memory_size, uint32_t script_memory_size)
{
	if(module_memory_size == 0 || script_memory_size == 0)
		return -1;

	int32_t module_id = -1;
	modulestore_t* module_store = assign_modulestore(&module_id);
	if(module_store == NULL)
		return -1;

	void* script_memory = malloc(script_memory_size);
	if(script_memory == NULL) {
		module_store->is_used = 0;
		return -1;
	}

	kinowasm_handle_t handle = kinowasm_init();
	if(handle == NULL) {
		free(script_memory);
		module_store->is_used = 0;
		return -1;
	}

	kinowasm_assign_memory(handle, script_memory, script_memory_size);
	if(kinowasm_get_memory(handle) == NULL) {
		kinowasm_term(handle);
		free(script_memory);
		module_store->is_used = 0;
		return -1;
	}

	module_store->S = handle;
	module_store->script_memory = script_memory;
	module_store->script_memory_size = script_memory_size;
	module_store->module_memory_size = module_memory_size;
	module_store->is_dirty = 0;
	return module_id;
}

kinowasm_result_t push_wasm_module(int32_t module_id, const char* module_name, const void* module_data, size_t module_data_size)
{
	_try{
		modulestore_t * module_store = find_modulestore(module_id);
		_throwif(INVALID_ARGUMENT, module_store == NULL || module_store->S == NULL);
		_throwif(ERR_FILENOTOPEN, module_name == NULL || module_data == NULL || module_data_size == 0);
		_throwif(ERR_OUTOFMEMORY, !grow_pushinfo(module_store));

		store_t* store = (store_t*)module_store->S;
		/* アリーナ一本化: 専用 module_memory を確保せず store の storememory に直接 decode する
		 * (relocate スキップ)。一時データ (chunks / module_t 配列) は load 内で free-list へ返却され
		 * リサイクルされる。module_memory は持たないので push_info には NULL を記録。 */
		modulepushinfo_t push_info = {
			.funcs_len = store->funcs.len,
			.tables_len = store->tables.len,
			.memorys_len = store->memorys.len,
			.globals_len = store->globals.len,
			.elements_len = store->elements.len,
			.datas_len = store->datas.len,
			.moduletable_len = store->moduletable.len,
			.module_memory = NULL,
		};

		kinowasm_result_t err = kinowasm_load_module_from_memory(module_store->S, (void*)module_data, module_data_size, module_name, store->storememory);
		if(_is_error(err)) {
			rollback_modulepush(module_store, &push_info);
			_throw(err);
		}

		module_store->push_infos[module_store->push_info_len++] = push_info;
		module_store->is_dirty = 1;
	}
	_catch:
	return _result;
}

kinowasm_result_t invoke_wasm_module(int32_t module_id, const char* module_name, const char* function_name, kinowasm_args_t* args)
{
	_try{
		modulestore_t * module_store = find_modulestore(module_id);
		_throwif(INVALID_ARGUMENT, module_store == NULL || module_store->S == NULL);
		_throwif(INVALID_ARGUMENT, module_name == NULL || function_name == NULL || args == NULL);

		if(module_store->is_dirty) {
			store_t* store = (store_t*)module_store->S;
			_throwiferr(rebuild_extra_func_type(store));
			_throwiferr(validate_function_parameter(module_store->S));
			module_store->is_dirty = 0;
		}

		_throwiferr(kinowasm_invoke(module_store->S, module_name, function_name, args));
	}
	_catch:
	return _result;
}

kinowasm_result_t pop_wasm_module(int32_t module_id)
{
	_try{
		modulestore_t * module_store = find_modulestore(module_id);
		_throwif(INVALID_ARGUMENT, module_store == NULL || module_store->S == NULL);
		_throwif(INVALID_ARGUMENT, module_store->push_info_len == 0);

		modulepushinfo_t push_info = module_store->push_infos[module_store->push_info_len - 1];
		_throwiferr(rollback_modulepush(module_store, &push_info));
		free(push_info.module_memory);
		module_store->push_info_len--;
		module_store->is_dirty = 1;
	}
	_catch:
	return _result;
}

void destroy_wasm_module(int32_t module_id)
{
	modulestore_t* module_store = find_modulestore(module_id);
	if(module_store == NULL)
		return;

	while(module_store->push_info_len != 0) {
		if(_is_error(pop_wasm_module(module_id)))
			break;
	}

	if(module_store->S != NULL)
		kinowasm_term(module_store->S);

	free(module_store->script_memory);
	free(module_store->push_infos);
	memset(module_store, 0, sizeof(*module_store));
}

static kinowasm_result_t check_validate_char(char param_type, resulttype_t rt, size_t index)
{
	_try{
		switch(param_type) {
		case 'v':
			if(rt.len != 0) {
				_throw(INVALID_ARGUMENT);
			}
			break;
		case 'i':
			if(rt.len <= index) {
				_throw(INVALID_ARGUMENT);
			}
			if(kinowasm_array_at(rt, index) != TYPE_VAL_I32) {
				_throw(INVALID_ARGUMENT);
			}
			break;
		case 'j':
			if(rt.len <= index) {
				_throw(INVALID_ARGUMENT);
			}
			if(kinowasm_array_at(rt, index) != TYPE_VAL_I64) {
				_throw(INVALID_ARGUMENT);
			}
			break;
		case 'f':
			if(rt.len <= index) {
				_throw(INVALID_ARGUMENT);
			}
			if(kinowasm_array_at(rt, index) != TYPE_VAL_F32) {
				_throw(INVALID_ARGUMENT);
			}
			break;
		case 'd':
			if(rt.len <= index) {
				_throw(INVALID_ARGUMENT);
			}
			if(kinowasm_array_at(rt, index) != TYPE_VAL_F64) {
				_throw(INVALID_ARGUMENT);
			}
			break;
		default:
			_throw(INVALID_ARGUMENT);
		}
	}
	_catch:
	return _result;
}

static kinowasm_result_t validate_parameter(const char* p, functiontype_t* func_type)
{
	_try{
		kinowasm_result_t final_err = INVALID_ARGUMENT;
		while(p != NULL) {
			const char* next_p = strchr(p, ':');
			size_t current_len = next_p ? (size_t)(next_p - p) : strlen(p);

			kinowasm_result_t step_err = 0;
			/* Check return type */
			step_err = check_validate_char(p[0], func_type->rt2, 0);
			if(!_is_error(step_err)) {
				/* Check parameter count */
				if(current_len - 1 == func_type->rt1.len) {
					/* Check each parameter */
					for(size_t j = 1; j < current_len; j++) {
						step_err = check_validate_char(p[j], func_type->rt1, j - 1);
						if(_is_error(step_err))
							break;
					}
				} else {
					step_err = INVALID_ARGUMENT;
				}
			}
			if(!_is_error(step_err)) {
				final_err = 0;
				break;
			}
			p = next_p ? next_p + 1 : NULL;
		}
_throw(final_err);
	}
	_catch:
	return _result;
}

static kinowasm_result_t validate_module_import(import_t* import, kinowasm_extrafuncs_t* extra_func_table, moduletable_t* moduletable)
{
	_try{
		for(uint32_t i = 0; i < extra_func_table->len; i++) {
			if(COMPARE_STRING(import->module.data, kinowasm_array_at((*extra_func_table), i).module) && COMPARE_STRING(import->name.data, kinowasm_array_at((*extra_func_table), i).name)) {
				printf("Validate:%s...", import->name.data);
				if(kinowasm_array_at((*extra_func_table), i).reserved != NULL) {
					const char* param_type_total = (const char*)kinowasm_array_at((*extra_func_table), i).reserved;
					functiontype_t* func_type = &kinowasm_array_at(moduletable->module->origin_module.types, import->d.functypeidx);
					kinowasm_result_t err = validate_parameter(param_type_total, func_type);
					if(_is_error(err)) {
						printf("NG\nInvalid parameter type or count. Function: %s, Defined: %s\n", import->name.data, param_type_total);
						_throw(err);
					}
				}
				printf("OK\n");
				break;
			}
		}
	}
	_catch:
	return _result;
}

kinowasm_result_t validate_function_parameter(kinowasm_handle_t handle)
{
	_try{
		printf("----Validate use function parameter type with extra function table.----\n");
		store_t* store = (store_t*)handle;
		kinowasm_extrafuncs_t* extra_func_table = kinowasm_settings.extra_func_table;
		kinowasm_array_foreach(moduletable, moduletable_t, store->moduletable) {
			printf("Module: %s\n", moduletable->name.data);
			kinowasm_array_foreach(import, import_t, moduletable->module->origin_module.imports) {
				if(import->d.kind == IMPORTDESC_FUNC) {
					_throwiferr(validate_module_import(import, extra_func_table, moduletable));
				}
			}
		}
		printf("----Validate use function parameter type with extra function table end.----\n");
	}
	_catch:
	return _result;
}

void register_standard_func(void)
{
	const kinowasm_extrafunc_t extra_table[] = {
		{ SYSTEM_FUNCTION, "Test1", test1, (void*)"vi" },
		{ SYSTEM_FUNCTION, "Pow", cmdpow, (void*)"iii" },
		{ SYSTEM_FUNCTION, "Console", console, (void*)"vi" },
		{ SYSTEM_FUNCTION, "_tzset_js", tz_set, (void*)"viiii" },
		{ SYSTEM_FUNCTION, "GetCurrentTime", get_current_time, (void*)"f" },
		{ SYSTEM_FUNCTION, "Wait", wait, (void*)"vf" },
		{ SYSTEM_FUNCTION, "emscripten_notify_memory_growth", emscripten_notify_memory_growth, (void*)"vi" },
		{ SYSTEM_FUNCTION, "segfault", segfault, (void*)"v" },
		{ SYSTEM_FUNCTION, "alignfault", alignfault, (void*)"v" },
		{ WASI_MODULE, "fd_close", fd_close, (void*)"ii" },
		{ WASI_MODULE, "fd_fdstat_get", fd_fdstat_get, (void*)"iii" },
		{ WASI_MODULE, "fd_seek", fd_seek, (void*)"iijii" },
		{ WASI_MODULE, "fd_tell", fd_tell, (void*)"iii" },
		{ WASI_MODULE, "fd_write", fd_write, (void*)"iiiii" },
		{ WASI_MODULE, "fd_read", fd_read, (void*)"iiiii" },
		{ WASI_MODULE, "fd_pread", fd_pread, (void*)"iiiiji" },
		{ WASI_MODULE, "fd_filestat_get", fd_filestat_get, (void*)"iii" },
		{ WASI_MODULE, "fd_prestat_get", fd_prestat_get, (void*)"iii" },
		{ WASI_MODULE, "fd_prestat_dir_name", fd_prestat_dir_name, (void*)"iiii" },
		{ WASI_MODULE, "fd_pwrite", fd_pwrite, (void*)"iiiiji" },
		{ WASI_MODULE, "path_open", path_open, (void*)"iiiiiijjii" },
		{ WASI_MODULE, "path_filestat_get", path_filestat_get, (void*)"iiiiii" },
		{ WASI_MODULE, "path_create_directory", path_create_directory, (void*)"iiii" },
		{ WASI_MODULE, "path_remove_directory", path_remove_directory, (void*)"iiii" },
		{ WASI_MODULE, "path_unlink_file", path_unlink_file, (void*)"iiii" },
		{ WASI_MODULE, "path_rename", path_rename, (void*)"iiiiiii" },
		{ WASI_MODULE, "fd_readdir", fd_readdir, (void*)"iiiiji" },
		{ WASI_MODULE, "path_link", path_link, (void*)"iiiiiiii" },
		{ WASI_MODULE, "path_symlink", path_symlink, (void*)"iiiiii" },
		{ WASI_MODULE, "path_readlink", path_readlink, (void*)"iiiiiii" },
		{ WASI_MODULE, "environ_sizes_get", environ_sizes_get, (void*)"iii" },
		{ WASI_MODULE, "environ_get", environ_get, (void*)"iii" },
		{ WASI_MODULE, "args_sizes_get", args_sizes_get, (void*)"iii" },
		{ WASI_MODULE, "args_get", args_get, (void*)"iii" },
		{ WASI_MODULE, "proc_exit", proc_exit, (void*)"vi" },
		{ WASI_MODULE, "clock_time_get", clock_time_get, (void*)"iiji" },
		{ WASI_MODULE, "clock_res_get", clock_res_get, (void*)"iii" },
		{ WASI_MODULE, "random_get", random_get, (void*)"iii" },

		/* ----- ENOSYS stubs (wasi-libc 全 import を満たすための binding) ----- */
		{ WASI_MODULE, "fd_advise",                wasi_stub_enosys,      (void*)"iijji" },
		{ WASI_MODULE, "fd_allocate",              wasi_stub_enosys,      (void*)"iijj" },
		{ WASI_MODULE, "fd_datasync",              wasi_stub_enosys,      (void*)"ii" },
		{ WASI_MODULE, "fd_fdstat_set_flags",      wasi_stub_enosys,      (void*)"iii" },
		{ WASI_MODULE, "fd_fdstat_set_rights",     wasi_stub_enosys,      (void*)"iijj" },
		{ WASI_MODULE, "fd_filestat_set_size",     wasi_stub_enosys,      (void*)"iij" },
		{ WASI_MODULE, "fd_filestat_set_times",    wasi_stub_enosys,      (void*)"iijji" },
		{ WASI_MODULE, "fd_renumber",              fd_renumber_impl,      (void*)"iii" },
		{ WASI_MODULE, "fd_sync",                  wasi_stub_enosys,      (void*)"ii" },
		{ WASI_MODULE, "path_filestat_set_times",  wasi_stub_enosys,      (void*)"iiiiijji" },
		{ WASI_MODULE, "poll_oneoff",              wasi_stub_enosys,      (void*)"iiiii" },
		{ WASI_MODULE, "sched_yield",              wasi_sched_yield_stub, (void*)"i" },
		{ WASI_MODULE, "sock_accept",              wasi_stub_enosys,      (void*)"iiii" },
		{ WASI_MODULE, "sock_recv",                wasi_stub_enosys,      (void*)"iiiiiii" },
		{ WASI_MODULE, "sock_send",                wasi_stub_enosys,      (void*)"iiiiii" },
		{ WASI_MODULE, "sock_shutdown",            wasi_sock_shutdown_stub, (void*)"iii" },
	};
	kinowasm_register_extra_func(extra_table, sizeof(extra_table) / sizeof(extra_table[0]));
}
