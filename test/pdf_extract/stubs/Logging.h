#pragma once

template <typename... Args>
inline void pdfTestLog(const Args&...) {}

#define LOG_ERR(...) pdfTestLog(__VA_ARGS__)
#define LOG_INF(...) pdfTestLog(__VA_ARGS__)
#define LOG_DBG(...) pdfTestLog(__VA_ARGS__)
