/**
 * @file dt.h
 * @authors Xingli Li
 * @date 2023-07-29
 * @brief Portable boolean type and constants.
 */
#ifndef DT_H
#define DT_H

// define bool if not defined
#ifndef bool
    typedef short bool;
#define true 1
#define false 0
#endif

#define TRUE true
#define FALSE false

#endif // DT_H
