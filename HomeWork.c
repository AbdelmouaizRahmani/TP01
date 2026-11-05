#include <stdio.h>

int main()
{
    int date, day, month, year;

    printf("Enter the date (DDMMYYYY): ");
    scanf("%d", &date);

    day = date / 1000000;
    month = (date / 10000) % 100;
    year = date % 10000;

    if(date == 0)
    {
        printf("Invlid date!");
        return 0;
    }
    else if(date < 0)
    {
        date *= -1;
    }
    else if(day == 0)
    {
        printf("Invalid day!");
        return 0;
    }

    switch(month)
    {
        case 1:
        case 3:
        case 5:
        case 7: 
        case 8:
        case 10:
        case 12:
            if(day > 30) printf("Invalid day!");
            else
            {
                printf("So %d ==> day:%d, month:%d, year:%d", date, day, month, year);
            }
            break;
        case 4:
        case 6:
        case 9:
        case 11:
            if(day > 31) printf("Invalid day!");
            else
            {
                printf("So %d ==> day:%d, month:%d, year:%d", date, day, month, year);
            }
            break;
        case 2:
            if(day > 29) printf("Invalid day!");
            else
            {
                printf("So %d ==> day:%d, month:%d, year:%d", date, day, month, year);
            }
            break;
        default:
            printf("Invalid month!");
            return 0;
            break;
    }
    
    return 0;

}