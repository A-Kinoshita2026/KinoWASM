//#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <string.h>
#include "systemmemory.h"
#include "winapi.h"
#include "kinowasm.h"

#define STATE_TEXT_NONE 0x00
#define STATE_TEXT_MESSAGE_SHOW 0x01
#define STATE_TEXT_MESSAGE_DONE 0x02

#define ADVENTURE_MODULE_NAME "adventure"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif

typedef kinowasm_array(uint16_t) u16string_t;
typedef kinowasm_array(u16string_t) u16stringarray_t;

static const float FPS_TIME = 0.016666668f;

static kinowasm_array(uint16_t) text_table_data = { 0 };
static kinowasm_array(uint32_t) text_start_position = { 0 };
static float prev_time = 0.0f;

typedef struct {
	u16stringarray_t message;
	u16stringarray_t ruby;
	u16string_t charname;
	u16string_t voice;
	uint32_t showtextcount;
} message_t;

enum ReadState
{
	TEXT,
	COMMAND,
	RUBY,
	VOICE,
	NAME,
};

message_t current_message = { { 0 }, { 0 }, { 0 }, { 0 }, 0 };
static float time_text_show = 0.0f;
static float text_show_speed_time = 0.02f;
static int advstate = 0;

#define MAX(a, b) \
	(((a) > (b)) ? (a) : (b))
#define MIN(a, b) \
	(((a) < (b)) ? (a) : (b))

static void* param_string = NULL;
static const wchar_t* get_wstring(kinowasm_callinfo_t* call, int32_t n)
{
	if (param_string)
		kinowasm_mem_free(param_string);

	wchar_t c;
	kinowasm_read_memory(call, n, &c, sizeof(c));
	int32_t count = 0;
	while (c != '\0') {
		count++;
		kinowasm_read_memory(call, n + count * sizeof(c), &c, sizeof(c));
	}
	param_string = (wchar_t*)kinowasm_mem_malloc(count * sizeof(c) + sizeof(c));
	kinowasm_read_memory(call, n, param_string, count * sizeof(c));
	((wchar_t*)param_string)[count] = '\0';
	return (const wchar_t*)param_string;
}
static const char* get_string(kinowasm_callinfo_t* call, int32_t n)
{
	if (param_string)
		kinowasm_mem_free(param_string);

	char c;
	kinowasm_read_memory(call, n, &c, sizeof(c));
	int32_t count = 0;
	while (c != '\0') {
		count++;
		kinowasm_read_memory(call, n + count * sizeof(c), &c, sizeof(c));
	}
	param_string = (char*)kinowasm_mem_malloc(count * sizeof(c) + sizeof(c));
	kinowasm_read_memory(call, n, param_string, count * sizeof(c));
	((char*)param_string)[count] = '\0';
	return (const char*)param_string;
}

static kinowasm_result_t append_char(u16string_t* string, uint16_t c)
{
	if (string->capacity <= string->len) {
		if (string->len == 0) {
			kinowasm_array_init_from(*string);
			kinowasm_array_grow_from(*string, 1);
		} else {
			kinowasm_array_grow_from(*string, string->len * 2);
		}
	}
	string->data[string->len] = c;
	string->len++;
	return 0;
}

static kinowasm_result_t append_message(u16stringarray_t* ary, u16string_t* str)
{
	if (ary->capacity == ary->len) {
		if (ary->len == 0) {
			kinowasm_array_init_from(*ary);
			kinowasm_array_grow_from(*ary, 1);
		} else {
			kinowasm_array_grow_from(*ary, ary->len * 2);
		}
	}
	ary->data[ary->len] = *str;
	ary->len++;
	return 0;
}

static void clean_message(message_t* message)
{
	kinowasm_array_foreach(msg, u16string_t, message->message)
		kinowasm_array_term_from(*msg);

	kinowasm_array_foreach(rby, u16string_t, message->ruby)
		kinowasm_array_term_from(*rby);

	message->charname.len = 0;
	message->voice.len = 0;
	message->message.len = 0;
	message->ruby.len = 0;
	message->showtextcount = 0;
}

static void parse_talk(const uint16_t* sourceString, message_t* output)
{
	enum ReadState state = TEXT;

	if (wcslen((const wchar_t*)sourceString) != 0)
	{
		uint8_t isRuby = 0;
		u16string_t message = { 0 };
		u16string_t ruby = { 0 };
		const uint16_t* chara = sourceString;
		while (*chara != u'\0' && *chara != u'\t') {
			// BOMを無視する
			if (*chara == 0xFEFF)
				continue;

			// コマンドモード
			if (state == COMMAND) {
				if (*chara == 'N') {
					// 名前モードへ
					state = NAME;
				} else if (*chara == 'R') {
					// ルビモードへ
					state = RUBY;
					message.len = 0;
					ruby.len = 0;
					isRuby = 0;
				} else if (*chara == 'V') {
					// ボイスモードへ
					state = VOICE;
				} else {
					// テキストモードへ戻す
					state = TEXT;
				}
			} else if (state == VOICE) {
				// ボイスモード
				if (*chara == '(') {
					// パラメータ開始(何もしない)
				} else if (*chara == ')') {
					// ボイスモード終了
					append_char(&output->voice, u'\0');
					state = TEXT;
				} else {
					// ボイス名
					append_char(&output->voice, *chara);
				}
			} else if (state == RUBY) {
				// ルビモード
				if (*chara == '(') {
					// ここからルビ
					isRuby = 1;
				} else if (*chara == ')') {
					// ルビモード終了
					append_char(&message, u'\0');
					append_char(&ruby, u'\0');
					append_message(&output->message, &message);
					append_message(&output->ruby, &ruby);
					state = TEXT;
				} else {
					if (isRuby) {
						// ルビ
						append_char(&ruby, *chara);
					} else {
						// 本文
						append_char(&message, *chara);
					}
				}
			} else if (state == NAME) {
				// 名前モード
				if (*chara == '(') {
					// パラメータ開始(何もしない)
				} else if (*chara == ')') {
					// 名前モード終了
					append_char(&output->charname, u'\0');
					state = TEXT;
				} else {
					// キャラ名
					append_char(&output->charname, *chara);
				}
			} else {
				if (*chara == '#') {
					// コマンドモードへ移行
					state = COMMAND;
				} else if (*chara != '\r' && *chara != '\0' && *chara != '\t') {
					u16string_t msg;
					u16string_t rby = { 0 };
					kinowasm_array_new_from(msg, 2);
					kinowasm_array_at(msg, 0) = *chara;
					kinowasm_array_at(msg, 1) = u'\0';
					kinowasm_array_init_from(rby);
					append_message(&output->message, &msg);
					append_message(&output->ruby, &rby);
				}
			}
			chara++;
		}
	}
}

static kinowasm_result_t adv_update(kinowasm_callinfo_t* call)
{
	float delta = get_running_time() - prev_time;
	prev_time = get_running_time();
	switch (advstate) {
	case STATE_TEXT_MESSAGE_SHOW:
		time_text_show += delta;
		if (time_text_show >= text_show_speed_time) {
			time_text_show = 0.0f;
			wprintf(L"%s", kinowasm_array_at(current_message.message, current_message.showtextcount).data);
			current_message.showtextcount++;
			if (current_message.showtextcount == current_message.message.len) {
				wprintf(L" ▼");
				advstate = STATE_TEXT_MESSAGE_DONE;
			}
		}
		break;
	case STATE_TEXT_MESSAGE_DONE:
		if (get_key_status(VK_RETURN) != 0) {
			clean_message(&current_message);
			advstate = STATE_TEXT_NONE;
			wprintf(L"\b \n\n");
		}
		break;
	default:
		break;
	}
	system_wait(MAX(FPS_TIME - delta, 0));
	return 0;
}

static kinowasm_result_t get_adv_state(kinowasm_callinfo_t* call)
{
	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i32 = advstate;
	return 0;
}

static kinowasm_result_t fadein(kinowasm_callinfo_t* call)
{
	wprintf(L"CallFadeIn\n");
	return 0;
}

static kinowasm_result_t fadeout(kinowasm_callinfo_t* call)
{
	wprintf(L"CallFadeOut\n");
	return 0;
}

static kinowasm_result_t load_text_table(kinowasm_callinfo_t* call)
{
	int32_t t = call->args->data[0].val.num.i32;
	const wchar_t* filename = get_wstring(call, t);
	FILE* fp = NULL;
	if ((fp = _wfopen(filename, L"rb")) != NULL) {
		change_system_memory();
		_fseeki64(fp, 0, SEEK_END);
		if (text_table_data.len != 0)
			kinowasm_array_term_from(text_table_data);

		if (kinowasm_array_new_from(text_table_data, (size_t)_ftelli64(fp)) == 0) {
			_fseeki64(fp, 0, SEEK_SET);
			fread(text_table_data.data, sizeof(uint8_t), text_table_data.len, fp);
			fclose(fp);
			uint32_t textCount = 1;
			kinowasm_array_foreach(c, uint16_t, text_table_data) {
				if (*c == u'\t')
					textCount++;

			}
			if (text_start_position.len != 0)
				kinowasm_array_term_from(text_start_position);

			if (kinowasm_array_new_from(text_start_position, textCount) == 0) {
				kinowasm_array_at(text_start_position, 0) = 0;
				uint32_t idx = 1;
				uint32_t pos = 0;
				kinowasm_array_foreach(c, uint16_t, text_table_data) {
					if (*c == u'\t')
						kinowasm_array_at(text_start_position, idx++) = pos + 1;

					pos++;
				}
			}
			wprintf(L"[%s] Loaded, TableCount: %d\n", filename, textCount);
			call->rets->data[0].type = TYPE_VAL_I32;
			call->rets->data[0].val.num.i32 = 0;
			return 0;
		}
	}
	wprintf(L"Load failed.\n");
	call->rets->data[0].type = TYPE_VAL_I32;
	call->rets->data[0].val.num.i32 = 1;
	return 0;
}

static kinowasm_result_t set_text_message(kinowasm_callinfo_t* call)
{
	uint32_t textIndex = (uint32_t)call->args->data[0].val.num.i32;
	if (textIndex < text_start_position.len) {
		//wprintf(L"Pos: %d\n", KARRAY_AT(textStartPosition, textIndex));
		const uint16_t* messageSource = (const uint16_t*)&kinowasm_array_at(text_table_data, kinowasm_array_at(text_start_position, textIndex));
		parse_talk(messageSource, &current_message);
		if(current_message.charname.len != 0)
			wprintf(L"【%s】\n", current_message.charname.data);

		advstate = STATE_TEXT_MESSAGE_SHOW;
	}
	return 0;
}

static kinowasm_result_t set_message_speed(kinowasm_callinfo_t* call)
{
	float wait = call->args->data[0].val.num.f32;
	text_show_speed_time = wait;
	return 0;
}

void regist_adventure_func(void)
{
	const kinowasm_extrafunc_t extraTable[] = {
		{ ADVENTURE_MODULE_NAME, "ADVUpdate", adv_update, NULL },
		{ ADVENTURE_MODULE_NAME, "GetADVState", get_adv_state, NULL },
		{ ADVENTURE_MODULE_NAME, "FadeIn", fadein, NULL },
		{ ADVENTURE_MODULE_NAME, "FadeOut", fadeout, NULL },
		{ ADVENTURE_MODULE_NAME, "LoadTextTable", load_text_table, NULL },
		{ ADVENTURE_MODULE_NAME, "SetTextMessage", set_text_message, NULL },
		{ ADVENTURE_MODULE_NAME, "SetMessageSpeed", set_message_speed, NULL },
	};
	kinowasm_register_extra_func(extraTable, sizeof(extraTable) / sizeof(extraTable[0]));
}

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif
