#include <stdio.h>

int main()
{
    int number = 0;
    long fact = 1;

    printf("Enter the number: ");
    scanf("%d", &number);

    if(number < 0)
    {
        printf("The number must be positive!");
        return 0;
    }
    else if(number <= 1)
    {
        fact = 1;
    }
    else
    {
        for(int i = number; i > 0; i--)
        {
            fact *= i;
        }
    }

    printf("So %d! equals %ld", number, fact);

    return 0;

}