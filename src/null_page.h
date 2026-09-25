// NBA JAM: On Fire Edition - surviving a missing asset
//
// See null_page.cpp.

#pragma once

// Give guest address zero a page of its own, so that reading through a null
// pointer returns zeros instead of taking the process down. Call once the
// guest's memory arena exists.
void NbaMapNullPage();
