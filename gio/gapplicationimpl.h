#include "giotypes.h"

typedef struct _GApplicationImpl GApplicationImpl;

G_GNUC_INTERNAL
void                    g_application_impl_destroy                      (GApplicationImpl   *impl);

G_GNUC_INTERNAL
GApplicationImpl *      g_application_impl_register                     (GApplication       *application,
                                                                         const gchar        *appid,
                                                                         GApplicationFlags   flags,
                                                                         gboolean           *is_remote,
                                                                         GCancellable       *cancellable,
                                                                         GError            **error);

G_GNUC_INTERNAL
void                    g_application_impl_activate                     (GApplicationImpl   *impl,
                                                                         GVariant           *platform_data);

G_GNUC_INTERNAL
void                    g_application_impl_open                         (GApplicationImpl   *impl,
                                                                         GFile             **files,
                                                                         gint                n_files,
                                                                         const gchar        *hint,
                                                                         GVariant           *platform_data);

G_GNUC_INTERNAL
void                    g_application_impl_flush                        (GApplicationImpl   *impl);