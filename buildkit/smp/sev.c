/* Wake cores waiting in WFE (a UP kernel never executes SEV). */
int main(void)
{
	for (int i = 0; i < 1000; i++)
		__asm__ volatile("dsb\n\tsev" ::: "memory");
	return 0;
}
