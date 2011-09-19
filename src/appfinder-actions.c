/*
 * Copyright (C) 2011 Nick Schermer <nick@xfce.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>
#include <xfconf/xfconf.h>

#include <src/appfinder-actions.h>
#include <src/appfinder-private.h>



static void xfce_appfinder_actions_finalize (GObject              *object);
static void xfce_appfinder_actions_free     (XfceAppfinderAction  *action);
static void xfce_appfinder_actions_load     (XfceAppfinderActions *actions);
static void xfce_appfinder_actions_save     (XfceAppfinderActions *actions,
                                             gboolean              save_actions);
static void xfce_appfinder_actions_changed  (XfconfChannel        *channel,
                                             const gchar          *prop_name,
                                             const GValue         *value,
                                             XfceAppfinderActions *actions);



struct _XfceAppfinderActionsClass
{
  GObjectClass __parent__;
};

struct _XfceAppfinderActions
{
  GObject __parent__;

  XfconfChannel *channel;
  gulong         property_watch_id;

  GSList        *actions;
};

typedef enum
{
  XFCE_APPFINDER_ACTION_TYPE_PREFIX,
  XFCE_APPFINDER_ACTION_TYPE_REGEX
}
AppfinderActionType;

struct _XfceAppfinderAction
{
  AppfinderActionType  type;

  gint                 unique_id;
  gchar               *pattern;
  gchar               *command;

  GRegex              *regex;
};



G_DEFINE_TYPE (XfceAppfinderActions, xfce_appfinder_actions, G_TYPE_OBJECT)



static void
xfce_appfinder_actions_class_init (XfceAppfinderActionsClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = xfce_appfinder_actions_finalize;
}



static void
xfce_appfinder_actions_init (XfceAppfinderActions *actions)
{
  actions->channel = xfconf_channel_get ("xfce4-appfinder");

  xfce_appfinder_actions_load (actions);

  actions->property_watch_id =
    g_signal_connect (G_OBJECT (actions->channel), "property-changed",
                      G_CALLBACK (xfce_appfinder_actions_changed), actions);
}



static void
xfce_appfinder_actions_finalize (GObject *object)
{
  XfceAppfinderActions *actions = XFCE_APPFINDER_ACTIONS (object);

  g_signal_handler_disconnect (actions->channel, actions->property_watch_id);

  g_slist_foreach (actions->actions, (GFunc) xfce_appfinder_actions_free, NULL);
  g_slist_free (actions->actions);

  (*G_OBJECT_CLASS (xfce_appfinder_actions_parent_class)->finalize) (object);
}



static void
xfce_appfinder_actions_free (XfceAppfinderAction *action)
{
  g_free (action->pattern);
  g_free (action->command);

  if (action->regex != NULL)
    g_regex_unref (action->regex);

  g_slice_free (XfceAppfinderAction, action);
}



static void
xfce_appfinder_actions_load_defaults (XfceAppfinderActions *actions)
{
  guint                i;
  XfceAppfinderAction *action;
  XfceAppfinderAction  defaults[] =
  {
    /* default actions, sorted */
    { XFCE_APPFINDER_ACTION_TYPE_REGEX, 0,
      "^(file|http|https):\\/\\/(.*)$",
      "exo-open \\0",
      NULL },
    { XFCE_APPFINDER_ACTION_TYPE_PREFIX, 0,
      "!",
      "exo-open --launch TerminalEmulator %s",
      NULL },
    { XFCE_APPFINDER_ACTION_TYPE_PREFIX, 0,
      "!w",
      "exo-open --launch WebBrowser http://en.wikipedia.org/wiki/%s",
      NULL },
    { XFCE_APPFINDER_ACTION_TYPE_PREFIX, 0,
      "#",
      "exo-open --launch TerminalEmulator man %s",
      NULL }
  };

  APPFINDER_DEBUG ("loaded default actions");

  for (i = 0; i < G_N_ELEMENTS (defaults); i++)
    {
      action = g_slice_new0 (XfceAppfinderAction);
      action->type = defaults[i].type;
      action->unique_id = i + 1;
      action->pattern = g_strdup (defaults[i].pattern);
      action->command = g_strdup (defaults[i].command);

      actions->actions = g_slist_prepend (actions->actions, action);
    }
}



static gint
xfce_appfinder_actions_sort (gconstpointer a,
                             gconstpointer b)
{
  const XfceAppfinderAction *action_a = a;
  const XfceAppfinderAction *action_b = b;

  if (action_a->type != action_b->type)
    return action_a->type == XFCE_APPFINDER_ACTION_TYPE_PREFIX ? -1 : 1;

  /* reverse the order so prefixes are properly matched */
  return -g_strcmp0 (action_a->pattern, action_b->pattern);
}



static void
xfce_appfinder_actions_load (XfceAppfinderActions *actions)
{
  XfceAppfinderAction *action;
  gchar                prop[32];
  gint                 type;
  gchar               *pattern, *command;
  const GValue        *value;
  guint                i;
  gint                 unique_id;
  GPtrArray           *array;

  if (xfconf_channel_has_property (actions->channel, "/actions"))
    {
      array = xfconf_channel_get_arrayv (actions->channel, "/actions");
      if (G_LIKELY (array != NULL))
        {
          for (i = 0; i < array->len; i++)
            {
              value = g_ptr_array_index (array, i);
              appfinder_assert (value != NULL);
              unique_id = g_value_get_int (value);

              g_snprintf (prop, sizeof (prop), "/actions/action-%d/type", unique_id);
              type = xfconf_channel_get_int (actions->channel, prop, -1);
              if (type < XFCE_APPFINDER_ACTION_TYPE_PREFIX
                  || type > XFCE_APPFINDER_ACTION_TYPE_REGEX)
                continue;

              g_snprintf (prop, sizeof (prop), "/actions/action-%d/pattern", unique_id);
              pattern = xfconf_channel_get_string (actions->channel, prop, NULL);

              g_snprintf (prop, sizeof (prop), "/actions/action-%d/command", unique_id);
              command = xfconf_channel_get_string (actions->channel, prop, NULL);

              if (pattern != NULL && command != NULL)
                {
                  action = g_slice_new0 (XfceAppfinderAction);
                  action->type = type;
                  action->unique_id = unique_id;
                  action->pattern = pattern;
                  action->command = command;

                  actions->actions = g_slist_prepend (actions->actions, action);
                }
              else
                {
                  g_free (pattern);
                  g_free (command);
                }
            }

          xfconf_array_free (array);
        }
    }
  else
    {
      xfce_appfinder_actions_load_defaults (actions);

      xfce_appfinder_actions_save (actions, TRUE);
    }

  actions->actions = g_slist_sort (actions->actions, xfce_appfinder_actions_sort);

  APPFINDER_DEBUG ("loaded %d actions", g_slist_length (actions->actions));
}



static void
xfce_appfinder_actions_save (XfceAppfinderActions *actions,
                             gboolean              save_actions)
{
  XfceAppfinderAction *action;
  GSList              *li;
  GValue              *value;
  GPtrArray           *array;
  gchar                prop[32];

  if (actions->property_watch_id > 0)
    g_signal_handler_block (actions->channel, actions->property_watch_id);

  array = g_ptr_array_new ();

  for (li = actions->actions; li != NULL; li = li->next)
    {
      value = g_new0 (GValue, 1);
      g_value_init (value, G_TYPE_INT);

      action = li->data;
      g_value_set_int (value, action->unique_id);
      g_ptr_array_add (array, value);

      if (save_actions)
        {
          g_snprintf (prop, sizeof (prop), "/actions/action-%d/type", action->unique_id);
          xfconf_channel_set_int (actions->channel, prop, action->type);

          g_snprintf (prop, sizeof (prop), "/actions/action-%d/pattern", action->unique_id);
          xfconf_channel_set_string (actions->channel, prop, action->pattern);

          g_snprintf (prop, sizeof (prop), "/actions/action-%d/command", action->unique_id);
          xfconf_channel_set_string (actions->channel, prop, action->command);
        }
    }

  xfconf_channel_set_arrayv (actions->channel, "/actions", array);

  xfconf_array_free (array);

  if (actions->property_watch_id > 0)
    g_signal_handler_unblock (actions->channel, actions->property_watch_id);
}



static void
xfce_appfinder_actions_changed (XfconfChannel        *channel,
                                const gchar          *prop_name,
                                const GValue         *value,
                                XfceAppfinderActions *actions)
{
  gint                 unique_id;
  gchar                field[32];
  GSList              *li;
  XfceAppfinderAction *action;

  if (prop_name == NULL)
    return;

  if (strcmp (prop_name, "/actions") == 0)
    {

    }
  else if (sscanf (prop_name, "/actions/action-%d/%30s",
                   &unique_id, field) == 2)
    {
      for (li = actions->actions; li != NULL; li = li->next)
        {
          action = li->data;

          if (action->unique_id == unique_id)
            {
              if (strcmp (field, "type") == 0
                  && G_VALUE_HOLDS_INT (value))
                {
                  action->type = g_value_get_int (value);
                }
              else if (strcmp (field, "pattern") == 0
                       && G_VALUE_HOLDS_STRING (value))
                {
                  g_free (action->pattern);
                  action->pattern = g_value_dup_string (value);

                  if (action->regex != NULL)
                    {
                      g_regex_unref (action->regex);
                      action->regex = NULL;
                    }
                }
              else if (strcmp (field, "command") == 0
                       && G_VALUE_HOLDS_STRING (value))
                {
                  g_free (action->command);
                  action->command = g_value_dup_string (value);
                }

              break;
            }
        }
    }
}



static gchar *
xfce_appfinder_actions_expand_command (XfceAppfinderAction *action,
                                       const gchar         *text)
{
  GString     *string;
  const gchar *p;
  gsize        len;
  gchar       *trim;

  if (G_UNLIKELY (action->command == NULL))
    return NULL;

  string = g_string_sized_new (128);
  len = strlen (action->pattern);

  for (p = action->command; *p != '\0'; ++p)
    {
      if (G_UNLIKELY (p[0] == '%' && p[1] != '\0'))
        {
          switch (*++p)
            {
            case 's':
              trim = g_strdup (text + len);
              g_string_append (string, g_strchug (trim));
              g_free (trim);
              break;

            case 'S':
              g_string_append (string, text);
              break;

            case '%':
              g_string_append_c (string, '%');
              break;
            }
        }
      else
        {
          g_string_append_c (string, *p);
        }
    }

  return g_string_free (string, FALSE);
}



XfceAppfinderActions *
xfce_appfinder_actions_get (void)
{
  static XfceAppfinderActions *actions = NULL;

  if (G_LIKELY (actions != NULL))
    {
      g_object_ref (G_OBJECT (actions));
    }
  else
    {
      actions = g_object_new (XFCE_TYPE_APPFINDER_ACTIONS, NULL);
      g_object_add_weak_pointer (G_OBJECT (actions), (gpointer) &actions);
      APPFINDER_DEBUG ("allocate actions");
    }

  return actions;
}



XfceAppfinderActionsResult
xfce_appfinder_actions_execute (XfceAppfinderActions  *actions,
                                const gchar           *text,
                                GdkScreen             *screen,
                                GError               **error)
{
  GSList                     *li;
  XfceAppfinderAction        *action;
  XfceAppfinderActionsResult  result = XFCE_APPFINDER_ACTIONS_NOTHING_FOUND;
  GError                     *err = NULL;
  gchar                      *cmd = NULL;
  GMatchInfo                 *match_info = NULL;
  gchar                      *expanded;

  appfinder_return_val_if_fail (error != NULL && *error == NULL, XFCE_APPFINDER_ACTIONS_NOTHING_FOUND);
  appfinder_return_val_if_fail (GDK_IS_SCREEN (screen), XFCE_APPFINDER_ACTIONS_NOTHING_FOUND);
  appfinder_return_val_if_fail (XFCE_IS_APPFINDER_ACTIONS (actions), XFCE_APPFINDER_ACTIONS_NOTHING_FOUND);
  appfinder_return_val_if_fail (text != NULL, XFCE_APPFINDER_ACTIONS_NOTHING_FOUND);

  for (li = actions->actions;
       result == XFCE_APPFINDER_ACTIONS_NOTHING_FOUND && li != NULL;
       li = li->next)
    {
      action = li->data;

      switch (action->type)
        {
        case XFCE_APPFINDER_ACTION_TYPE_PREFIX:
          if (g_str_has_prefix (text, action->pattern))
            {
              cmd = xfce_appfinder_actions_expand_command (action, text);
              result = XFCE_APPFINDER_ACTIONS_SUCCEED;
            }
          break;

        case XFCE_APPFINDER_ACTION_TYPE_REGEX:
          if (action->regex == NULL)
            {
              action->regex = g_regex_new (action->pattern, 0, 0, &err);
              if (action->regex == NULL)
                {
                  g_message ("Failed to create regex for \"%s\": %s",
                             action->pattern, err->message);
                  g_error_free (err);

                  break;
                }
            }

          if (g_regex_match (action->regex, text, 0, &match_info))
            {
              /* expand the command string */
              cmd = g_match_info_expand_references (match_info, action->command, &err);

              if (G_UNLIKELY (err != NULL))
                *error = err;
            }

          if (match_info != NULL)
            g_match_info_free (match_info);

          break;
        }
    }

  if (result == XFCE_APPFINDER_ACTIONS_SUCCEED
      && cmd != NULL
      && *error == NULL)
    {
      /* also expand variables... */
      expanded = xfce_expand_variables (cmd, NULL);
      g_free (cmd);
      cmd = expanded;

      APPFINDER_DEBUG ("spawn command \"%s\"", cmd);

      if (xfce_spawn_command_line_on_screen (screen, cmd, FALSE, FALSE, error))
        result = XFCE_APPFINDER_ACTIONS_SUCCEED;
      else
        result = XFCE_APPFINDER_ACTIONS_ERROR;
    }

  g_free (cmd);

  return result;
}