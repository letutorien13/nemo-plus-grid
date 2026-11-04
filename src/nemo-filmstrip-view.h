/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/* nemo-filmstrip-view.h - Filmstrip / Pellicule view (Windows XP style)
 *
 * Part of nemo+ fork.
 */

#ifndef NEMO_FILMSTRIP_VIEW_H
#define NEMO_FILMSTRIP_VIEW_H

#include "nemo-view.h"

#define NEMO_TYPE_FILMSTRIP_VIEW nemo_filmstrip_view_get_type()
#define NEMO_FILMSTRIP_VIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), NEMO_TYPE_FILMSTRIP_VIEW, NemoFilmstripView))
#define NEMO_FILMSTRIP_VIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), NEMO_TYPE_FILMSTRIP_VIEW, NemoFilmstripViewClass))
#define NEMO_IS_FILMSTRIP_VIEW(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), NEMO_TYPE_FILMSTRIP_VIEW))
#define NEMO_IS_FILMSTRIP_VIEW_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), NEMO_TYPE_FILMSTRIP_VIEW))
#define NEMO_FILMSTRIP_VIEW_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), NEMO_TYPE_FILMSTRIP_VIEW, NemoFilmstripViewClass))

#define NEMO_FILMSTRIP_VIEW_ID "OAFIID:Nemo_File_Manager_Filmstrip_View"

typedef struct NemoFilmstripViewDetails NemoFilmstripViewDetails;

typedef struct {
	NemoView parent_instance;
	NemoFilmstripViewDetails *details;
} NemoFilmstripView;

typedef struct {
	NemoViewClass parent_class;
} NemoFilmstripViewClass;

GType nemo_filmstrip_view_get_type (void);
void  nemo_filmstrip_view_register (void);

#endif /* NEMO_FILMSTRIP_VIEW_H */
