#ifndef SPRING_LINT_OMP_H_
#define SPRING_LINT_OMP_H_

/* Provide minimal OpenMP stubs for static analysis on systems where the real
   omp.h is not on the include path.  If a compiler-supplied omp.h has already
   been included (detected via its guard macro), skip everything. */
#if !defined(__OMP_H)
#define __OMP_H

typedef struct omp_lock_t { void *_lk; } omp_lock_t;

#ifdef __cplusplus
extern "C" {
#endif

int omp_get_thread_num(void);
int omp_get_num_threads(void);
int omp_get_max_threads(void);
void omp_set_num_threads(int num_threads);
void omp_set_dynamic(int dynamic_threads);
int omp_get_dynamic(void);
void omp_init_lock(omp_lock_t *lock);
void omp_set_lock(omp_lock_t *lock);
void omp_unset_lock(omp_lock_t *lock);
void omp_destroy_lock(omp_lock_t *lock);
int omp_test_lock(omp_lock_t *lock);

#ifdef __cplusplus
}
#endif

#endif /* !defined(__OMP_H) */
#endif /* SPRING_LINT_OMP_H_ */
