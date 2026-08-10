extern "C" int __wrap_printf(const char* format, ...) {
    (void)format;
    return 0;
}
