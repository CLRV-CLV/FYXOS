#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*unary_function)(int);

enum result_kind {
    RESULT_RAW = 0,
    RESULT_TRANSFORMED = 1,
};

struct computation {
    int values[5];
    size_t count;
    enum result_kind kind;
    uint32_t tag;
};

static int twice(int value)
{
    return value * 2;
}

static int increment(int value)
{
    return value + 1;
}

static int sum_values(const struct computation *input)
{
    int total = 0;
    for (size_t i = 0; i < input->count; i++)
        total += input->values[i];
    return total;
}

_Static_assert(sizeof(uint32_t) == 4, "FYXOS requires 32-bit uint32_t");

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    volatile int runtime_first = 4;
    struct computation input = {
        .values = { runtime_first, 2, 3, 4, 5 },
        .count = 5,
        .kind = RESULT_RAW,
        .tag = 0x46595831u,
    };
    int expected[5] = { 4, 2, 3, 4, 5 };
    if (argc < 1 || !argv || !argv[0] || input.tag != 0x46595831u)
        return 1;
    if (memcmp(input.values, expected, sizeof(expected)) != 0)
        return 2;

    unary_function operation = argc > 0 ? twice : increment;
    int result = operation(sum_values(&input));
    int *third = &input.values[2];
    input.kind = RESULT_TRANSFORMED;
    if (result != 36 || *third != 3 || input.kind != RESULT_TRANSFORMED)
        return 3;

    puts("GCC_C11_OK");
    return result + 6;
}
