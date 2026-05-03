#ifndef SPRING_QVZ_DISTORTION_H_
#define SPRING_QVZ_DISTORTION_H_

#include <stdint.h>

#define DISTORTION_MANHATTAN 1
#define DISTORTION_MSE 2
#define DISTORTION_LORENTZ 3
#define DISTORTION_CUSTOM 4

namespace spring {
namespace qvz {

struct distortion_t {
  double *distortion;
  uint8_t symbols;
  int type;
};

struct distortion_t *alloc_distortion_matrix(uint8_t symbols);
void free_distortion_matrix(struct distortion_t *);

struct distortion_t *generate_distortion_matrix(uint8_t symbols, int type);
struct distortion_t *gen_mse_distortion(uint8_t symbols);
struct distortion_t *gen_manhattan_distortion(uint8_t symbols);
struct distortion_t *gen_lorentzian_distortion(uint8_t symbols);
struct distortion_t *gen_custom_distortion(uint8_t symbols,
                                           const char *filename);

double get_distortion(struct distortion_t *dist, uint8_t x, uint8_t y);

void print_distortion(struct distortion_t *dist);

} // namespace qvz
} // namespace spring

#endif
