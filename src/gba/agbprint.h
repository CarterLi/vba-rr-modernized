#ifndef VBA_AGBPRINT_H
#define VBA_AGBPRINT_H

#pragma once

extern bool agbPrintIsEnabled();
extern void agbPrintReset();
extern void agbPrintSetAvailable(bool);
extern bool agbPrintWrite(u32, u16);
extern void agbPrintFlush();

#endif // VBA_AGBPRINT_H
