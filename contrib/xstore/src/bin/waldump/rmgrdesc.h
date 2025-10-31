/* -------------------------------------------------------------------------
 *
 * rmgrdesc.h
 * 
 *
 *
 * Portions Copyright (c) 2024-2025 Tianyi Cloud Technology Co., Ltd
 * 
 *
 * IDENTIFICATION
 * src/bin/waldump/rmgrdesc.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef XSTORE_RMGRDESC_H
#define XSTORE_RMGRDESC_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

typedef struct RmgrDescData
{
	const char *rm_name;
	void		(*rm_desc) (StringInfo buf, XLogReaderState *record);
	const char *(*rm_identify) (uint8 info);
} RmgrDescData;

extern const RmgrDescData *GetRmgrDesc(RmgrId rmid);

#endif							/* XSTORE_RMGRDESC_H */
