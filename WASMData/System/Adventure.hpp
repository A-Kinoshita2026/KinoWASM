#pragma once

#include "Fiber.hpp"
#include "ScriptSystem.hpp"

#define STATE_TEXT_MESSAGE_SHOW 0x01
#define STATE_TEXT_MESSAGE_DONE 0x02

Fiber<void> SetTextWait(int textNo)
{
	SetTextMessage(textNo);
	while (GetADVState() == STATE_TEXT_MESSAGE_SHOW || GetADVState() == STATE_TEXT_MESSAGE_DONE) {
		co_yield{};
	}
}

#define TextSet(i) co_await SetTextWait(i)

Fiber<void> ADVStart();

void Starter()
{
	auto co = ADVStart();
	for(;;) {
		co.resume();
		if(co.isDone()){ break; }
		ADVUpdate();
	}
}
