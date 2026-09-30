/*
 * pgo_deprobe.c
 *
 * MSVC PGO の instrument 版 exe から「値プローブ」呼び出しだけを除去するバイナリパッチャ。
 *
 * 背景:
 *   /GENPROFILE は間接呼び出しや除算などに値プローブ (__PogoProbe*Value*) の call を
 *   挿入する。この call はターゲット値を r10/r11 に積んで __PogoProbe... を呼ぶが、
 *   呼び出し規約により rcx/rdx/r8/r9 を破壊する。core の dispatch は musttail で
 *   rcx=pc / rdx=sp / r8=mem / r9=r0 を引き回しているため、値プローブの call が
 *   これらを壊し、instrument 版が 0xC0000005 でクラッシュする。
 *
 *   一方、エッジカウントは `inc qword ptr [r12+off]` という直接命令で記録され、
 *   関数呼び出しを伴わないので musttail と無関係。値プローブの引数積み込み
 *   (movsxd r10/mov r11) も scratch レジスタなので handler に影響しない。
 *   よって「値プローブの call (E8 rel32, 5byte) だけを NOP(0x90) に潰す」だけで、
 *   カウント記録を無傷に保ったまま instrument を完走させられる (.pgc が正しく出る)。
 *
 * 使い方:
 *   pgo_deprobe <exe> <map>
 *     exe       : /GENPROFILE /MAP でリンクした instrument 実行ファイル (in-place で書換)
 *     map       : 同リンクの .map。__PogoProbe*Value* の Rva+Base をここから読む。
 *
 * 注意:
 *   - 除去対象は名前に "Value" を含む値プローブのみ (__PogoProbeValue /
 *     __PogoProbeTemplatedValue / __PogoProbeFilteredValue と各 MDS 版)。
 *     __PogoNopCount や __PogoEntryThunk 等 (カウント/初期化) は触らない。
 *   - PE32+ (x64) 前提。実行属性 (IMAGE_SCN_CNT_CODE) を持つ全セクションを走査する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
/* windows.h を丸ごと取り込まず SetConsoleOutputCP だけ前方宣言する (依存を増やさない)。 */
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int code_page);
#endif

#define MAX_PROBES 64

/* ファイル全体を読み込む。*out_len にバイト数。失敗で NULL。 */
static unsigned char* read_file(const char* path, long* out_len)
{
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return NULL; }
	unsigned char* buf = (unsigned char*)malloc((size_t)n);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	fclose(f);
	*out_len = n;
	return buf;
}

/* map から __PogoProbe*Value* の Rva+Base (16 桁 hex) を集める。戻り値=個数。 */
static int parse_map_probes(const char* map_path, uint64_t* probes, int max)
{
	FILE* f = fopen(map_path, "r");
	if (!f) return -1;
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "__PogoProbe")) continue;   /* プローブ行のみ */
		if (!strstr(line, "Value")) continue;          /* 値プローブのみ (NopCount 等は除外) */
		/* 行中の 16 桁 hex トークン (= Rva+Base) を探す。strtok は line を破壊するが
		   上の strstr 判定は済んでいるので問題ない。 */
		uint64_t addr = 0;
		for (char* tok = strtok(line, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
			if (strlen(tok) != 16) continue;
			int ishex = 1;
			for (int i = 0; i < 16; i++) {
				char c = tok[i];
				if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
					ishex = 0; break;
				}
			}
			if (ishex) { addr = strtoull(tok, NULL, 16); break; }
		}
		if (addr && n < max) probes[n++] = addr;
	}
	fclose(f);
	return n;
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
	/* 本ソースは /utf-8 でコンパイルされ、日本語 narrow 文字列は UTF-8 バイトで埋め込まれる。
	 * コンソール既定 CP (CP932 等) のままだと出力が文字化けするため UTF-8 に切り替える。 */
	SetConsoleOutputCP(65001u);
#endif
	if (argc < 3) {
		fprintf(stderr, "usage: pgo_deprobe <exe> <map>\n");
		return 1;
	}
	uint64_t probes[MAX_PROBES];
	int nprobes = parse_map_probes(argv[2], probes, MAX_PROBES);
	if (nprobes <= 0) {
		fprintf(stderr, "pgo_deprobe: map から値プローブが見つかりません (%d)\n", nprobes);
		return 2;
	}

	long exe_len = 0;
	unsigned char* exe = read_file(argv[1], &exe_len);
	if (!exe) { fprintf(stderr, "pgo_deprobe: %s を読めません\n", argv[1]); return 3; }

	/* ---- PE32+ (x64) ヘッダ解析 ---- */
	if (exe_len < 0x40 || exe[0] != 'M' || exe[1] != 'Z') {
		fprintf(stderr, "pgo_deprobe: MZ ヘッダ無し\n"); free(exe); return 4;
	}
	uint32_t e_lfanew = *(uint32_t*)(exe + 0x3C);
	if ((long)e_lfanew + 24 > exe_len) { fprintf(stderr, "bad e_lfanew\n"); free(exe); return 4; }
	unsigned char* nt = exe + e_lfanew;
	if (memcmp(nt, "PE\0\0", 4) != 0) { fprintf(stderr, "pgo_deprobe: PE 署名無し\n"); free(exe); return 5; }
	uint16_t num_sec  = *(uint16_t*)(nt + 6);    /* FileHeader.NumberOfSections */
	uint16_t opt_size = *(uint16_t*)(nt + 20);   /* FileHeader.SizeOfOptionalHeader */
	unsigned char* opt = nt + 24;                /* OptionalHeader 先頭 */
	uint16_t magic = *(uint16_t*)(opt + 0);
	if (magic != 0x20B) { fprintf(stderr, "pgo_deprobe: PE32+ ではありません\n"); free(exe); return 6; }
	uint64_t image_base = *(uint64_t*)(opt + 24); /* PE32+ OptionalHeader.ImageBase */
	unsigned char* sec = opt + opt_size;          /* セクションヘッダ配列 */

	/* ---- 実行コードセクションを走査し、値プローブ call を NOP 化 ---- */
	int patched = 0;
	for (int s = 0; s < num_sec; s++) {
		unsigned char* sh = sec + s * 40;            /* IMAGE_SECTION_HEADER (40 byte) */
		uint32_t va       = *(uint32_t*)(sh + 12);   /* VirtualAddress (RVA) */
		uint32_t raw_size = *(uint32_t*)(sh + 16);   /* SizeOfRawData */
		uint32_t raw_ptr  = *(uint32_t*)(sh + 20);   /* PointerToRawData */
		uint32_t chars    = *(uint32_t*)(sh + 36);   /* Characteristics */
		if (!(chars & 0x20)) continue;               /* IMAGE_SCN_CNT_CODE のみ */
		if ((long)raw_ptr + raw_size > exe_len) continue;

		for (uint32_t i = 0; i + 5 <= raw_size; i++) {
			if (exe[raw_ptr + i] != 0xE8) continue;  /* call rel32 */
			int32_t rel = *(int32_t*)(exe + raw_ptr + i + 1);
			uint32_t instr_rva = va + i;
			uint64_t target = image_base + (uint64_t)(instr_rva + 5) + (int64_t)rel;
			for (int p = 0; p < nprobes; p++) {
				if (target == probes[p]) {
					for (int k = 0; k < 5; k++) exe[raw_ptr + i + k] = 0x90; /* NOP */
					patched++;
					i += 4; /* この命令分を飛ばす (i++ と合わせて 5 byte 進む) */
					break;
				}
			}
		}
	}

	FILE* f = fopen(argv[1], "wb");
	if (!f) { fprintf(stderr, "pgo_deprobe: %s に書き込めません\n", argv[1]); free(exe); return 7; }
	fwrite(exe, 1, (size_t)exe_len, f);
	fclose(f);
	free(exe);

	fprintf(stderr, "pgo_deprobe: 値プローブ call を %d 箇所 NOP 化しました (プローブ関数 %d 個)\n",
		patched, nprobes);
	return 0;
}
