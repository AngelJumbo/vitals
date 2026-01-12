#include "modules.h"
// this is just a test to see if i can commit

float cpu_perc() {
  static long double a[7] = {0};
  long double b[7], sum;
  //if(skip_read) return 0;

  memcpy(b, a, sizeof(b));

  FILE *fp = fopen("/proc/stat", "r");
  if (!fp) {
      perror("Failed to open /proc/stat");
      return -1;
  }

  if (fscanf(fp, "cpu  %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
             &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) != 7) {
      fclose(fp);
      return -1;
  }
  fclose(fp);

  if (b[0] == 0) {
      return -1;
  }

  sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
        (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

  if (sum == 0) {
      return -1;
  }

  return (float)(100 * ((b[0] + b[1] + b[2] + b[5] + b[6]) -
                      (a[0] + a[1] + a[2] + a[5] + a[6])) / sum);
}

