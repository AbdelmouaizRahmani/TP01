#include <stdio.h>
#include <math.h>

int main()
{
    float a, b, c, delta, x0, x1, x2, x3;

    printf("*** QUADRATIC EQUATIONS SOLVER ***\n\n");
    printf("So here you have the following equation: ");
    printf(" a * x^2 + b * x + c = 0 \n\n");

    printf("Enter the three numbers a, b and c: ");
    scanf("%f %f %f", &a, &b, &c);

    if(a == 0 && b == 0) 
    {
        printf("The equation is invalid!\n\n");
        return 0;
    }
    else if(a == 0 && b != 0)
    {
        printf("Then we the following equation: b * x3 + c = 0 \n");
        x3 = (-1 * c) / b;
        printf("So the solution to this equetion is: x3 = %.2f \n\n", x3);
    }
    else if(a != 0)
    {
        delta = pow(b, 2) - 4 * a * c;

        if(delta == 0)
        {
            x0 = (-1 * b) / (2 * a);
            printf("The equation has only one solution which is: x0 = %.2f \n\n", x0);
        }
        else if(delta > 0)
        {
            x1 = ((-1 * b) - sqrt(delta)) / (2 * a);
            x2 = ((-1 * b) + sqrt(delta)) / (2 * a);
            printf("The equation has two slutions which are: x1 = %.2f and x2 = %.2f \n\n", x1, x2);
        }
        else
        {
            printf("The equation has no valid solutions in the set of real numbers.\n\n");
            return 0;
        }
    }

    return 0;

}
