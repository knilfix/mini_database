#include <stdio.h>

int main(int argc, char *argv[])
{

    printf("Program name: %s\n", argv[0]);
    printf("You passed %d arguments: (s) \n", argc - 1);
    for (int i = 1; i < argc; i++)
    {
        printf("	[%d]: %s\n", i, argv[i]);
    }

    return 0;
}
