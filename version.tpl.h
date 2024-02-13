#ifndef VERSION_H_
#define VERSION_H_

#define COMPILE_TIME __DATE__"_"__TIME__

#define VERSION COMPILE_TIME "_" COMMIT_TAG

#endif   // end of define VERSION_H_
