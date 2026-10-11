#ifndef ESP_LOG_H
#define ESP_LOG_H

/*
 * Host stub for the ESP-IDF logging macros. Every line goes to stderr and
 * the last one is kept in stub_log_last as "<level> <tag>: <message>", so
 * tests can check what a driver said, and under which tag.
 */

extern char stub_log_last[256];

void stub_log(char level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) stub_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) stub_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) stub_log('D', tag, fmt, ##__VA_ARGS__)

#endif
