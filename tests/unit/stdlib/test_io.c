#include "../test_framework.h"
#include "../../../stdlib/io/io.h"
#include "base/xmalloc.h"

#include <string.h>
#include <unistd.h>

static char *read_via_stdin(const char *input, size_t *out_len) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return NULL;

    size_t input_len = strlen(input);
    ssize_t written = write(pipefd[1], input, input_len);
    close(pipefd[1]);
    if (written != (ssize_t) input_len) {
        close(pipefd[0]);
        return NULL;
    }

    int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0) {
        close(pipefd[0]);
        return NULL;
    }
    if (dup2(pipefd[0], STDIN_FILENO) < 0) {
        close(pipefd[0]);
        close(saved_stdin);
        return NULL;
    }
    close(pipefd[0]);

    char *buf = xr_io_read_stdin_all(out_len);

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
    return buf;
}

TEST(io_read_stdin_all_reads_entire_stream) {
    size_t len = 0;
    char *buf = read_via_stdin("hello\nworld", &len);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 11);
    ASSERT_STR_EQ(buf, "hello\nworld");
    xr_free(buf);
}

TEST(io_read_stdin_all_handles_empty_stream) {
    size_t len = 123;
    char *buf = read_via_stdin("", &len);
    ASSERT_NOT_NULL(buf);
    ASSERT_EQ_UINT(len, 0);
    ASSERT_STR_EQ(buf, "");
    xr_free(buf);
}

TEST_MAIN_BEGIN()
RUN_TEST_SUITE("IO");
RUN_TEST(io_read_stdin_all_reads_entire_stream);
RUN_TEST(io_read_stdin_all_handles_empty_stream);
TEST_MAIN_END()
