unsigned long strlen(const char *s) {
    unsigned long i = 0;
    while (*s++ != '\0') {
        i++;
    }
    return i;
}

int isprint(int c) {
    return c >= 0x20 && c <= 0x7E;
}
