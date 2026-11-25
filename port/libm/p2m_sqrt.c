/* p2libm: sqrt and sqrtf. IEEE-754 square root is correctly rounded, so the instruction gives the
 * same bits everywhere. Built with -fno-math-errno, so these are the bare instruction. */
double sqrt(double x)
{
	return __builtin_sqrt(x);
}

float sqrtf(float x)
{
	return __builtin_sqrtf(x);
}
