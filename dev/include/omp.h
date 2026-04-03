#ifndef SPRING_LINT_OMP_H_
#define SPRING_LINT_OMP_H_

typedef int omp_lock_t;

#ifdef __cplusplus
extern "C" {
#endif

int omp_get_thread_num(void);
int omp_get_num_threads(void);
int omp_get_max_threads(void);
void omp_set_num_threads(int num_threads);
void omp_set_dynamic(int dynamic_threads);
void omp_init_lock(omp_lock_t *lock);
void omp_set_lock(omp_lock_t *lock);
void omp_unset_lock(omp_lock_t *lock);
int omp_test_lock(omp_lock_t *lock);

#ifdef __cplusplus
}
#endif

#endif