
#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "tvrq/rq_api.h"


/*
PCG Random Number Generation
http://www.pcg-random.org

I use global RNGs only.
*/
static uint64_t pcg32_state, pcg32_inc;

uint32_t pcg32_random()
{
    uint64_t oldstate = pcg32_state;
    pcg32_state = oldstate * 6364136223846793005ULL + pcg32_inc;
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((uint32_t)(-(int32_t)rot) & 31));
}

void pcg32_srandom(uint64_t seed, uint64_t seq)
{
	pcg32_state = 0U;
	pcg32_inc = (seq << 1u) | 1u;
	pcg32_random();
	pcg32_state += seed;
	pcg32_random();
}

uint32_t pcg32_boundedrand(uint32_t bound)
{
    uint32_t threshold = (uint32_t)(-(int32_t)bound) % bound;

    for (;;) {
        uint32_t r = pcg32_random();
        if (r >= threshold)
            return r % bound;
    }
}


/*
source_count = s#
parity_count = p#
lost_count = l#
each_size = d#
*/
int main(int argc, char* argv[]){
	char *p;
	char *source_buf, *parity_buf, *inter_buf, *work_buf;
	int i, j, max, rv;
	int source_count = 1000, parity_count = 200, lost_count = 0;
	int input_count;
	int *order_buf;
	unsigned int *int_p;
	size_t each_size = 64000;
	size_t source_size, parity_size, inter_size;
	size_t malloc_size, input_size, output_size;
	double value_kb, value_mb, input_speed, output_speed;
	clock_t start, finish;
	double duration;
	RqInterWorkMem* interWork = NULL;
	RqInterProgram* interProgram = NULL;
	RqOutWorkMem*   outWork = NULL;
	RqOutProgram*   outProgram = NULL;
	size_t interWorkSize, interProgSize, interSymCount;
	size_t outWorkSize, outProgSize;

	// read input
	for (i = 1; i < argc; i++){
		p = argv[i];
		//printf("argv[%d] = %s\n", i, p);

		if ( (p[0] == 's') || (p[0] == 'S') ){
			source_count = atoi(p + 1);
		} else if ( (p[0] == 'p') || (p[0] == 'P') ){
			parity_count = atoi(p + 1);
		} else if ( (p[0] == 'l') || (p[0] == 'L') ){
			lost_count = atoi(p + 1);
		} else if ( (p[0] == 'e') || (p[0] == 'E') ){
			each_size = atoi(p + 1);

		} else {	// invalid option
			printf("options: s#, p#, l#, e#\ns# or S# = source count (1 ~ 56403)\np# or P# = parity count\nl# or L# = loss count\ne# or E# = each data size (multiple of 4)\n");
			return 0;
		}
	}

	// check input
	if (source_count < 1)
		source_count = 1;
	if (source_count > 56403)
		source_count = 56403;
	if (parity_count <= 0)
		parity_count = 1;
	if ( (lost_count <= 0) || (lost_count > parity_count) )
		lost_count = parity_count;
	if (lost_count > source_count)
		lost_count = source_count;
	printf("source count = %d, parity count = %d, loss count = %d\n", source_count, parity_count, lost_count);
	if (each_size & 3)	// each data size must be multiple of 4.
		each_size = (each_size + 3) & ~3;
	value_kb = (double)each_size / 1000;
	value_mb = (double)each_size / 1000000.f;
	printf("each data size = %zd bytes = %g KB = %g MB\n", each_size, value_kb, value_mb);

	start = clock();
	printf("\n allocate memory and setup data ...\n");

	// allocate buffer
	source_size = each_size * source_count;
	source_buf = malloc(source_size);
	if (source_buf == NULL){
		printf("Failed: malloc, %zd\n", source_size);
		return 1;
	}
	value_kb = (double)source_size / 1000;
	value_mb = (double)source_size / 1000000.f;
	printf("source data size = %zd bytes = %g KB = %g MB\n", source_size, value_kb, value_mb);

	// fill source data with random
	max = (int)each_size / 4;
	for (i = 0; i < source_count; i++){
		int_p = (int *)(source_buf + each_size * i);
		pcg32_srandom(0, i);
		for (j = 0; j < max; j++)
			int_p[j] = pcg32_random();
	}

	// fill parity data with zero
	parity_size = each_size * parity_count;
	parity_buf = calloc(parity_size, 1);
	if (parity_buf == NULL){
		printf("Failed: malloc, %zd\n", parity_size);
		free(source_buf);
		return 1;
	}
	value_kb = (double)parity_size / 1000;
	value_mb = (double)parity_size / 1000000.f;
	printf("parity data size = %zd bytes = %g KB = %g MB\n", parity_size, value_kb, value_mb);

	finish = clock();
	duration = (double)(finish - start) / CLOCKS_PER_SEC;
	printf(" ... %.3f seconds\n", duration);

	printf("\n encode ...\n");
	start = clock();

    // Get various memory sizes from RQ API
	rv = RqInterGetMemSizes(source_count, parity_count, &interWorkSize, &interProgSize, &interSymCount);
	if (rv != 0){
		printf("Failed: RqInterGetMemSizes, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		return 1;
	}
	printf("interWorkSize = %zd, interProgSize = %zd, interSymCount = %zd\n",
			interWorkSize, interProgSize, interSymCount);

	// Create encoding interProgram
	interWork = malloc(interWorkSize);
	if (interWork == NULL){
		printf("Failed: malloc, %zd\n", interWorkSize);
		free(source_buf);
		free(parity_buf);
		return 1;
	}
	rv = RqInterInit(source_count, parity_count, interWork, interWorkSize);
	if (rv != 0){
		printf("Failed: RqInterInit, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		return 1;
	}
	rv = RqInterAddIds(interWork, 0, source_count);
	if (rv != 0){
		printf("Failed: RqInterAddIds, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		return 1;
	}
	interProgram = malloc(interProgSize);
	if (interProgram == NULL){
		printf("Failed: malloc, %zd\n", interProgSize);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		return 1;
	}
	rv = RqInterCompile(interWork, interProgram, interProgSize);
	if (rv != 0){
		printf("Failed: RqInterCompile, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		return 1;
	}
	// temporary buffer for intermediate data
	inter_size = each_size * interSymCount;
	inter_buf = malloc(inter_size);
	if (inter_buf == NULL){
		printf("Failed: malloc, %zd\n", inter_size);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		return 1;
	}
	value_kb = (double)inter_size / 1000;
	value_mb = (double)inter_size / 1000000.f;
	printf("intermediate data size = %zd bytes = %g KB = %g MB\n", inter_size, value_kb, value_mb);

	// Create encoding intermediate block
	rv = RqInterExecute(interProgram, each_size, source_buf, source_size, inter_buf, inter_size);
	if (rv != 0){
		printf("Failed: RqInterExecute, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}

	// Create encoding output symbol Program
	rv = RqOutGetMemSizes(parity_count, &outWorkSize, &outProgSize);
	if (rv != 0){
		printf("Failed: RqOutGetMemSizes, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}
	printf("outWorkSize = %zd, outProgSize = %zd\n", outWorkSize, outProgSize);
	outWork = malloc(outWorkSize);
	if (outWork == NULL){
		printf("Failed: malloc, %zd\n", outWorkSize);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}
	outProgram = malloc(outProgSize);
	if (outProgram == NULL){
		printf("Failed: malloc, %zd\n", outProgSize);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		return 1;
	}
	rv = RqOutInit(source_count, outWork, outWorkSize);
	if (rv != 0){
		printf("Failed: RqOutInit, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}
	rv = RqOutAddIds(outWork, source_count, parity_count);
	if (rv != 0){
		printf("Failed: RqOutAddIds, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}
	rv = RqOutCompile(outWork, outProgram, outProgSize);
	if (rv != 0){
		printf("Failed: RqOutCompile, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}

	// Compute encoding
	rv = RqOutExecute(outProgram, each_size, inter_buf, parity_buf, parity_size);
	if (rv != 0){
		printf("Failed: RqOutExecute, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}

	finish = clock();
	duration = (double)(finish - start) / CLOCKS_PER_SEC;
	printf(" ... %.3f seconds\n", duration);

	free(interWork);	// No need encoder
	free(interProgram);
	free(inter_buf);
	free(outWork);
	free(outProgram);

	output_size = each_size * parity_count;
	value_mb = (double)source_size / 1000000.f;
	input_speed = value_mb / duration;
	output_speed = (double)output_size / 1000000.f / duration;
	printf("TvRQ Encoder(%g MB in %d pieces, %d parities): Input= %g MB/s, Output= %g MB/s\n",
			value_mb, source_count, parity_count, input_speed, output_speed);

	start = clock();
	printf("\n set %d random error and allocate memory ...\n", lost_count);

	// shuffle order of source and parity data
	malloc_size = (source_count + parity_count) * sizeof(int);	// available source and parity
	order_buf = malloc(malloc_size);
	if (order_buf == NULL){
		printf("Failed: malloc, %zd\n", malloc_size);
		free(source_buf);
		free(parity_buf);
		return 1;
	}
	max = source_count + parity_count;
	for (i = 0; i < max; i++)
		order_buf[i] = i;

	// swap index to select available data
	pcg32_srandom(0, 1);
	for (i = 0; i < source_count; i++){
		j = pcg32_boundedrand(source_count);
		if (j != i){
			rv = order_buf[j];
			order_buf[j] = order_buf[i];
			order_buf[i] = rv;
		}
	}
	max = source_count + lost_count;
	for (i = source_count; i < max; i++){
		j = source_count + pcg32_boundedrand(lost_count);
		if (j != i){
			rv = order_buf[j];
			order_buf[j] = order_buf[i];
			order_buf[i] = rv;
		}
	}
	if (parity_count > lost_count){
		max = source_count + parity_count;
		for (i = source_count + lost_count; i < max; i++){
			j = source_count + lost_count + pcg32_boundedrand(parity_count - lost_count);
			if (j != i){
				rv = order_buf[j];
				order_buf[j] = order_buf[i];
				order_buf[i] = rv;
			}
		}
	}

	// allocate working buffer
	malloc_size = each_size * (source_count + parity_count);
	work_buf = malloc(malloc_size);
	if (parity_buf == NULL){
		printf("Failed: malloc, %zd\n", malloc_size);
		free(source_buf);
		free(parity_buf);
		free(order_buf);
		return 1;
	}
	value_kb = (double)source_size / 1000;
	value_mb = (double)source_size / 1000000.f;
	printf("work data size = %zd bytes = %g KB = %g MB\n", malloc_size, value_kb, value_mb);

	// erase lost source data
	// index of available source data = order_buf[0 ~ source_count - lost_count]
	// index of lost source data = order_buf[source_count - lost_count ~ source_count - 1]
	for (i = source_count - lost_count; i < source_count; i++){
		j = order_buf[i];	// index of lost source data
		memset(source_buf + each_size * j, 0, each_size);	// zero fill
	}

	finish = clock();
	duration = (double)(finish - start) / CLOCKS_PER_SEC;
	printf(" ... %.3f seconds\n", duration);

	printf("\n decode ...\n");
	start = clock();

	// Get various memory sizes from RQ API
	rv = RqInterGetMemSizes(source_count, parity_count, &interWorkSize, &interProgSize, &interSymCount);
	if (rv != 0){
		printf("Failed: RqInterGetMemSizes, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(order_buf);
		free(work_buf);
		return 1;
	}
	printf("interWorkSize = %zd, interProgSize = %zd, interSymCount = %zd\n",
			interWorkSize, interProgSize, interSymCount);

	// Create decoding interProgram
	interWork = malloc(interWorkSize);
	if (interWork == NULL){
		printf("Failed: malloc, %zd\n", interWorkSize);
		free(source_buf);
		free(parity_buf);
		free(order_buf);
		free(work_buf);
		return 1;
	}
	rv = RqInterInit(source_count, parity_count, interWork, interWorkSize);
	if (rv != 0){
		printf("Failed: RqInterInit, %d\n", rv);
		free(source_buf);
		free(parity_buf);
		free(order_buf);
		free(work_buf);
		free(interWork);
		return 1;
	}
	interProgram = malloc(interProgSize);
	if (interProgram == NULL){
		printf("Failed: malloc, %zd\n", interProgSize);
		free(source_buf);
		free(parity_buf);
		free(order_buf);
		free(work_buf);
		free(interWork);
		return 1;
	}

	// input available source data at first
	max = source_count - lost_count;
	for (i = 0; i < max; i++){
		j = order_buf[i];	// index of available source data
		rv = RqInterAddIds(interWork, j, 1);
		if (rv != 0){
			printf("Failed: RqInterAddIds, %d\n", rv);
			free(source_buf);
			free(parity_buf);
			free(order_buf);
			free(work_buf);
			free(interWork);
			free(interProgram);
			return 1;
		}
		// copy availavle source data into work buffer
		memcpy(work_buf + each_size * i, source_buf + each_size * j, each_size);
	}
	printf("input %d available source data\n", max);

	// input available parity data until recovery is possible
	input_count = max;
	for (i = 0; i < parity_count; i++){
		j = order_buf[source_count + i];	// index of available parity data
		rv = RqInterAddIds(interWork, j, 1);
		if (rv != 0){
			printf("Failed: RqInterAddIds, %d\n", rv);
			free(source_buf);
			free(parity_buf);
			free(order_buf);
			free(work_buf);
			free(interWork);
			free(interProgram);
			return 1;
		}
		// copy availavle parity data into work buffer
		memcpy(work_buf + each_size * input_count, parity_buf + each_size * (j - source_count), each_size);

		input_count++;
		if (input_count >= source_count){	// check possible recovery
			rv = RqInterCompile(interWork, interProgram, interProgSize);
			if (rv == 0){	// got enough data to recover
				break;
			} else if (rv != RQ_ERR_INSUFF_IDS){
				printf("Failed: RqInterCompile, %d\n", rv);
				free(source_buf);
				free(parity_buf);
				free(order_buf);
				free(work_buf);
				free(interWork);
				free(interProgram);
				return 1;
			}
		}
	}
	printf("input %d available parity data\n", input_count - max);
	free(source_buf);	// No need source & parity data anymore
	free(parity_buf);

	if (rv != 0){
		printf("Failed: need more parity to recover, %d\n", input_count);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		return 1;
	}
	printf("ready to recover after %d input\n", input_count);

	// temporary buffer for intermediate data
	inter_size = each_size * interSymCount;
	inter_buf = malloc(inter_size);
	if (inter_buf == NULL){
		printf("Failed: malloc, %zd\n", inter_size);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		return 1;
	}

	// Create decoding intermediate block
	rv = RqInterExecute(interProgram, each_size, work_buf, each_size * input_count, inter_buf, inter_size);
	if (rv != 0){
		printf("Failed: RqInterExecute, %d\n", rv);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}

	// decrease work buffer to keep recovered data
	malloc_size = each_size * lost_count;
	p = realloc(work_buf, malloc_size);
	if (p != NULL)
		work_buf = p;

	// Create decoding output symbol Program
	rv = RqOutGetMemSizes(lost_count, &outWorkSize, &outProgSize);
	if (rv != 0){
		printf("Failed: RqOutGetMemSizes, %d\n", rv);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}
	printf("outWorkSize = %zd, outProgSize = %zd\n", outWorkSize, outProgSize);
	outWork = malloc(outWorkSize);
	if (outWork == NULL){
		printf("Failed: malloc, %zd\n", outWorkSize);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		return 1;
	}
	outProgram = malloc(outProgSize);
	if (outProgram == NULL){
		printf("Failed: malloc, %zd\n", outProgSize);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		return 1;
	}
	rv = RqOutInit(source_count, outWork, outWorkSize);
	if (rv != 0){
		printf("Failed: RqOutInit, %d\n", rv);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}

	// set IDs of lost source data
	// index of available source data = order_buf[0 ~ source_count - lost_count]
	// index of lost source data = order_buf[source_count - lost_count ~ source_count - 1]
	for (i = source_count - lost_count; i < source_count; i++){
		j = order_buf[i];	// index of lost source data
		rv = RqOutAddIds(outWork, j, 1);
		if (rv != 0){
			printf("Failed: RqOutAddIds, %d\n", rv);
			free(order_buf);
			free(work_buf);
			free(interWork);
			free(interProgram);
			free(inter_buf);
			free(outWork);
			free(outProgram);
			return 1;
		}
	}
	rv = RqOutCompile(outWork, outProgram, outProgSize);
	if (rv != 0){
		printf("Failed: RqOutCompile, %d\n", rv);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}

	// Compute decoding
	output_size = each_size * lost_count;
	rv = RqOutExecute(outProgram, each_size, inter_buf, work_buf, output_size);
	if (rv != 0){
		printf("Failed: RqOutExecute, %d\n", rv);
		free(order_buf);
		free(work_buf);
		free(interWork);
		free(interProgram);
		free(inter_buf);
		free(outWork);
		free(outProgram);
		return 1;
	}

	finish = clock();
	duration = (double)(finish - start) / CLOCKS_PER_SEC;
	printf(" ... %.3f seconds\n", duration);

	free(interWork);	// No need decoder
	free(interProgram);
	free(inter_buf);
	free(outWork);
	free(outProgram);

	value_kb = (double)output_size / 1000;
	value_mb = (double)output_size / 1000000.f;
	printf("lost data size = %zd bytes = %g KB = %g MB\n", output_size, value_kb, value_mb);
	output_speed = value_mb / duration;

	input_size = (uint64_t)each_size * input_count;
	value_kb = (double)input_size / 1000;
	value_mb = (double)input_size / 1000000.f;
	printf("input data size = %zd bytes = %g KB = %g MB\n", input_size, value_kb, value_mb);
	input_speed = value_mb / duration;
	printf("TvRQ Decoder(%g MB in %d pieces, %d losses): Input= %g MB/s, Output= %g MB/s\n",
			value_mb, input_count, lost_count, input_speed, output_speed);

	start = clock();
	printf("\n verify recovered data ...\n");

	max = (int)(each_size / 4);
	for (i = 0; i < lost_count; i++){
		int_p = (int *)(work_buf + each_size * i);
		j = order_buf[i + source_count - lost_count];	// index of lost source data
		pcg32_srandom(0, j);
		for (j = 0; j < max; j++){
			if (int_p[j] != pcg32_random())
				break;
		}
		if (j < max){
			printf("Failed: recovered data, %d\n", i);
			break;
		}
	}
	if (i == lost_count)
		printf("All data repaired !\n");
	free(order_buf);	// End comparison
	free(work_buf);

	finish = clock();
	duration = (double)(finish - start) / CLOCKS_PER_SEC;
	printf(" ... %.3f seconds\n", duration);

	return 0;
}

