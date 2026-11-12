#include <stdio.h>

int main()
{
    int date, day, month, year;

    printf("Enter the date(DDMMYYYY): ");
    scanf("%d", &date);

    day = date / 1000000;
    month = (date / 10000) % 100;
    year = date % 10000;

    if(date == 0)
    {
        printf("Invalid date!");
        return 0;
    }
    if(day == 0)
    {
        printf("Invalid day!");
        return 0;
    }
    if(date < 0)
    {
        date *= -1;
    }

    switch(month)
    {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
            if(day <= 30)
            {
                day += 1;
            }
            else if(day == 31)
            {
                day = 1;
                month += 1;
            }
            else
            {
                printf("Invalid day!");
                return 0;
            }
            break;
        case 12:
            if(day <= 30)
            {
                day += 1;
            }
            else if(day == 31)
            {
                day = 1;
                month = 1;
                year += 1;
            }
            else
            {
                printf("Invalid day!");
            }
            break;
        case 4:
        case 6:
        case 9:
        case 11:
            if(day <= 29)
            {
                day += 1;
            }
            else if(day == 30)
            {
                day = 1;
                month += 1;
            }
            else
            {
                printf("Invalid day!");
                return 0;
            }
            break;
        case 2:
            if(year % 4 == 0)
            {
                if(day <= 28)
                {
                    day += 1;
                }
                else if(day == 29)
                {
                    day = 1;
                    month += 1;
                }
                else
                {
                    printf("Invalid day!");
                    return 0;
                }
            }
            else
            {
                if(day <= 27)
                {
                    day += 1;
                }
                else if(day == 28)
                {
                    day = 1;
                    month += 1;
                }
                else
                {
                    printf("Invalid day!");
                    return 0;
                }
            }
            break;
        default:
            printf("Invalid date!");
            return 0;
    }

    printf("The date after one day %d/%d/%d", day, month, year);

    return 0;

}
