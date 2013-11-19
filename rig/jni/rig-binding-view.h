/*
 * Rig
 *
 * Copyright (C) 2013 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _RIG_BINDING_VIEW_H_
#define _RIG_BINDING_VIEW_H_

#include <rut.h>

extern RutType rig_binding_view_type;

typedef struct _RigBindingView RigBindingView;

RigBindingView *
rig_binding_view_new (RutContext *ctx);

#endif /* _RIG_BINDING_VIEW_H_ */
