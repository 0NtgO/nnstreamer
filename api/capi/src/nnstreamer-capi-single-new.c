/**
 * Copyright (c) 2019 Samsung Electronics Co., Ltd All Rights Reserved
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 */
/**
 * @file nnstreamer-capi-single-new.c
 * @date 29 Aug 2019
 * @brief NNStreamer/Single C-API Wrapper.
 *        This allows to invoke individual input frame with NNStreamer.
 * @see	https://github.com/nnsuite/nnstreamer
 * @author MyungJoo Ham <myungjoo.ham@samsung.com>
 * @author Parichay Kapoor <pk.kapoor@samsung.com>
 * @bug No known bugs except for NYI items
 */

#include <nnstreamer-single.h>
#include <nnstreamer-capi-private.h>
#include <nnstreamer_plugin_api.h>

#include "tensor_filter_single.h"

#define ML_SINGLE_MAGIC 0xfeedfeed

/**
 * @brief Default time to wait for an output in appsink (3 seconds).
 */
#define SINGLE_DEFAULT_TIMEOUT 3000

/**
 * @brief Global lock for single shot API
 * @detail This lock ensures that ml_single_close is thread safe. All other API
 *         functions use the mutex from the single handle. However for close,
 *         single handle mutex cannot be used as single handle is destroyed at
 *         close
 * @note This mutex is automatically initialized as it is statically declared
 */
G_LOCK_DEFINE_STATIC (magic);

/**
 * @brief Get valid handle after magic verification
 * @note handle's mutex (single_h->mutex) is acquired after this
 * @param[out] single_h The handle properly casted: (ml_single *).
 * @param[in] single The handle to be validated: (void *).
 * @param[in] reset Set TRUE if the handle is to be reset (magic = 0).
 */
#define ML_SINGLE_GET_VALID_HANDLE_LOCKED(single_h, single, reset) do { \
  G_LOCK (magic); \
  single_h = (ml_single *) single; \
  if (single_h->magic != ML_SINGLE_MAGIC) { \
    ml_loge ("The given param, single is invalid."); \
    G_UNLOCK (magic); \
    return ML_ERROR_INVALID_PARAMETER; \
  } \
  if (reset) \
    single_h->magic = 0; \
  g_mutex_lock (&single_h->mutex); \
  G_UNLOCK (magic); \
} while (0)

/**
 * @brief This is for the symmetricity with ML_SINGLE_GET_VALID_HANDLE_LOCKED
 * @param[in] single_h The casted handle (ml_single *).
 */
#define ML_SINGLE_HANDLE_UNLOCK(single_h) g_mutex_unlock (&single_h->mutex);

/** States for invoke thread */
typedef enum {
  IDLE = 0,           /**< ready to accept next input */
  RUNNING,            /**< running an input, cannot accept more input */
  JOIN_REQUESTED,     /**< should join the thread, will exit soon */
  ERROR               /**< error on thread, will exit soon */
} thread_state;

/** ML single api data structure for handle */
typedef struct
{
  GTensorFilterSingle *filter;  /**< tensor filter element */
  ml_tensors_info_s in_info;    /**< info about input */
  ml_tensors_info_s out_info;   /**< info about output */
  guint magic;                  /**< code to verify valid handle */

  GThread *thread;              /**< thread for invoking */
  GMutex mutex;                 /**< mutex for synchronization */
  GCond cond;                   /**< condition for synchronization */
  ml_tensors_data_h input;      /**< input received from user */
  ml_tensors_data_h *output;    /**< output to be sent back to user */
  guint timeout;                /**< timeout for invoking */
  thread_state state;           /**< current state of the thread */
  gboolean ignore_output;       /**< ignore and free the output */
  int status;                   /**< status of processing */
} ml_single;

/**
 * @brief thread to execute calls to invoke
 */
static void *
invoke_thread (void *arg)
{
  ml_single *single_h;
  GTensorFilterSingleClass *klass;
  GstTensorMemory in_tensors[NNS_TENSOR_SIZE_LIMIT];
  GstTensorMemory out_tensors[NNS_TENSOR_SIZE_LIMIT];
  ml_tensors_data_s *in_data, *out_data;
  int i, status = ML_ERROR_NONE;

  single_h = (ml_single *) arg;

  g_mutex_lock (&single_h->mutex);

  /** get the tensor_filter element */
  klass = g_type_class_peek (G_TYPE_TENSOR_FILTER_SINGLE);
  if (!klass) {
    single_h->state = ERROR;
    goto exit;
  }

  while (single_h->state <= RUNNING) {

    /** wait for data */
    while (single_h->state != RUNNING) {
      g_cond_wait (&single_h->cond, &single_h->mutex);
      if (single_h->state >= JOIN_REQUESTED)
        goto exit;
    }

    in_data = (ml_tensors_data_s *) single_h->input;

    /** Setup input buffer */
    for (i = 0; i < in_data->num_tensors; i++) {
      in_tensors[i].data = in_data->tensors[i].tensor;
      in_tensors[i].size = in_data->tensors[i].size;
      in_tensors[i].type = (tensor_type) single_h->in_info.info[i].type;
    }

    /** Setup output buffer */
    for (i = 0; i < single_h->out_info.num_tensors; i++) {
      /** memory will be allocated by tensor_filter_single */
      out_tensors[i].data = NULL;
      out_tensors[i].size =
          ml_tensor_info_get_size (&single_h->out_info.info[i]);
      out_tensors[i].type = (tensor_type) single_h->out_info.info[i].type;
    }
    g_mutex_unlock (&single_h->mutex);

    /** invoke the thread */
    if (klass->invoke (single_h->filter, in_tensors, out_tensors) == FALSE) {
      status = ML_ERROR_INVALID_PARAMETER;
      g_mutex_lock (&single_h->mutex);
      goto wait_for_next;
    }

    g_mutex_lock (&single_h->mutex);
    /** Allocate output buffer */
    if (single_h->ignore_output == FALSE) {
      status = ml_tensors_data_create_no_alloc (&single_h->out_info,
          single_h->output);
      if (status != ML_ERROR_NONE) {
        ml_loge ("Failed to allocate the memory block.");
        (*single_h->output) = NULL;
        goto wait_for_next;
      }

      /** set the result */
      out_data = (ml_tensors_data_s *) (*single_h->output);
      for (i = 0; i < single_h->out_info.num_tensors; i++) {
        out_data->tensors[i].tensor = out_tensors[i].data;
      }
    } else {
      /**
       * Caller of the invoke thread has returned back with timeout
       * so, free the memory allocated by the invoke as their is no receiver
       */
      for (i = 0; i < single_h->out_info.num_tensors; i++)
        g_free (out_tensors[i].data);
    }

    /** loop over to wait for the next element */
  wait_for_next:
    single_h->status = status;
    if (single_h->state == RUNNING)
      single_h->state = IDLE;
    g_cond_broadcast (&single_h->cond);
  }

exit:
  if (single_h->state != ERROR)
    single_h->state = IDLE;
  g_mutex_unlock (&single_h->mutex);
  return NULL;
}

/**
 * @brief Set the info for input/output tensors
 */
static int
ml_single_set_inout_tensors_info (GObject * object,
    const gchar * prefix, ml_tensors_info_s * tensors_info)
{
  int status = ML_ERROR_NONE;
  GstTensorsInfo info;
  gchar *str_dim, *str_type, *str_name;
  gchar *str_type_name, *str_name_name;

  ml_tensors_info_copy_from_ml (&info, tensors_info);

  /* Set input option */
  str_dim = gst_tensors_info_get_dimensions_string (&info);
  str_type = gst_tensors_info_get_types_string (&info);
  str_name = gst_tensors_info_get_names_string (&info);

  str_type_name = g_strdup_printf ("%s%s", prefix, "type");
  str_name_name = g_strdup_printf ("%s%s", prefix, "name");

  if (!str_dim || !str_type || !str_name || !str_type_name || !str_name_name) {
    status = ML_ERROR_INVALID_PARAMETER;
  } else {
    g_object_set (object, prefix, str_dim, str_type_name, str_type,
        str_name_name, str_name, NULL);
  }

  g_free (str_type_name);
  g_free (str_name_name);
  g_free (str_dim);
  g_free (str_type);
  g_free (str_name);

  gst_tensors_info_free (&info);

  return status;
}

/**
 * @brief Opens an ML model and returns the instance as a handle.
 */
int
ml_single_open (ml_single_h * single, const char *model,
    const ml_tensors_info_h input_info, const ml_tensors_info_h output_info,
    ml_nnfw_type_e nnfw, ml_nnfw_hw_e hw)
{
  ml_single *single_h;
  GObject *filter_obj;
  int status = ML_ERROR_NONE;
  GTensorFilterSingleClass *klass;
  ml_tensors_info_s *in_tensors_info, *out_tensors_info;
  bool available = false;
  bool valid = false;
  GError * error;

  check_feature_state ();

  /* Validate the params */
  if (!single) {
    ml_loge ("The given param, single is invalid.");
    return ML_ERROR_INVALID_PARAMETER;
  }

  /* init null */
  *single = NULL;

  in_tensors_info = (ml_tensors_info_s *) input_info;
  out_tensors_info = (ml_tensors_info_s *) output_info;

  /* Validate input tensor info. */
  if (input_info && !ml_tensors_info_is_valid (input_info, valid)) {
    ml_loge ("The given param, input tensor info is invalid.");
    return ML_ERROR_INVALID_PARAMETER;
  }

  /* Validate output tensor info. */
  if (output_info && !ml_tensors_info_is_valid (output_info, valid)) {
    ml_loge ("The given param, output tensor info is invalid.");
    return ML_ERROR_INVALID_PARAMETER;
  }

  /**
   * 1. Determine nnfw
   */
  if ((status = ml_validate_model_file (model, &nnfw)) != ML_ERROR_NONE)
    return status;

  /**
   * 2. Determine hw
   * @todo Now the param hw is ignored.
   * (Supposed CPU only) Support others later.
   */
  status = ml_check_nnfw_availability (nnfw, hw, &available);
  if (status != ML_ERROR_NONE)
    return status;

  if (!available) {
    ml_loge ("The given nnfw is not available.");
    status = ML_ERROR_NOT_SUPPORTED;
    return status;
  }

  /** Create ml_single object */
  single_h = g_new0 (ml_single, 1);
  g_assert (single_h);
  single_h->magic = ML_SINGLE_MAGIC;

  single_h->filter = g_object_new (G_TYPE_TENSOR_FILTER_SINGLE, NULL);
  single_h->timeout = SINGLE_DEFAULT_TIMEOUT;
  if (single_h->filter == NULL) {
    status = ML_ERROR_INVALID_PARAMETER;
    goto error;
  }
  filter_obj = G_OBJECT (single_h->filter);

  /**
   * 3. Construct a pipeline
   * Set the pipeline desc with nnfw.
   */
  switch (nnfw) {
    case ML_NNFW_TYPE_CUSTOM_FILTER:
      g_object_set (filter_obj, "framework", "custom", "model", model, NULL);
      break;
    case ML_NNFW_TYPE_TENSORFLOW_LITE:
      /* We can get the tensor meta from tf-lite model. */
      g_object_set (filter_obj, "framework", "tensorflow-lite",
          "model", model, NULL);
      break;
    case ML_NNFW_TYPE_TENSORFLOW:
      if (in_tensors_info && out_tensors_info) {
        status = ml_single_set_inout_tensors_info (filter_obj, "input",
            in_tensors_info);
        if (status != ML_ERROR_NONE)
          goto error;

        status = ml_single_set_inout_tensors_info (filter_obj, "output",
            out_tensors_info);
        if (status != ML_ERROR_NONE)
          goto error;

        g_object_set (filter_obj, "framework", "tensorflow",
            "model", model, NULL);
      } else {
        ml_loge ("To run the pipeline with tensorflow model, \
            input and output information should be initialized.");
        status = ML_ERROR_INVALID_PARAMETER;
        goto error;
      }
      break;
    default:
      /** @todo Add other fw later. */
      ml_loge ("The given nnfw is not supported.");
      status = ML_ERROR_NOT_SUPPORTED;
      goto error;
  }

  /* 4. Allocate */
  ml_tensors_info_initialize (&single_h->in_info);
  ml_tensors_info_initialize (&single_h->out_info);

  /* 5. Start the nnfw to get inout configurations if needed */
  klass = g_type_class_peek (G_TYPE_TENSOR_FILTER_SINGLE);
  if (!klass) {
    status = ML_ERROR_INVALID_PARAMETER;
    goto error;
  }
  if (klass->start (single_h->filter) == FALSE) {
    status = ML_ERROR_INVALID_PARAMETER;
    goto error;
  }

  /* 6. Set in/out configs and metadata */
  if (in_tensors_info) {
    /** set the tensors info here */
    if (!klass->input_configured (single_h->filter)) {
      status = ml_single_set_inout_tensors_info (filter_obj, "input",
          in_tensors_info);
      if (status != ML_ERROR_NONE)
        goto error;
    }
    status = ml_tensors_info_clone (&single_h->in_info, in_tensors_info);
    if (status != ML_ERROR_NONE)
      goto error;
  } else {
    ml_tensors_info_h in_info;

    if (!klass->input_configured (single_h->filter)) {
      ml_loge ("Failed to configure input info in filter.");
      status = ML_ERROR_INVALID_PARAMETER;
      goto error;
    }

    status = ml_single_get_input_info (single_h, &in_info);
    if (status != ML_ERROR_NONE) {
      ml_loge ("Failed to get the input tensor info.");
      goto error;
    }

    status = ml_tensors_info_clone (&single_h->in_info, in_info);
    ml_tensors_info_destroy (in_info);
    if (status != ML_ERROR_NONE)
      goto error;

    if (!ml_tensors_info_is_valid (&single_h->in_info, valid)) {
      ml_loge ("The input tensor info is invalid.");
      status = ML_ERROR_INVALID_PARAMETER;
      goto error;
    }
  }

  if (out_tensors_info) {
    /** set the tensors info here */
    if (!klass->output_configured (single_h->filter)) {
      status = ml_single_set_inout_tensors_info (filter_obj, "output",
          out_tensors_info);
      if (status != ML_ERROR_NONE)
        goto error;
    }
    status = ml_tensors_info_clone (&single_h->out_info, out_tensors_info);
    if (status != ML_ERROR_NONE)
      goto error;
  } else {
    ml_tensors_info_h out_info;

    if (!klass->output_configured (single_h->filter)) {
      ml_loge ("Failed to configure output info in filter.");
      status = ML_ERROR_INVALID_PARAMETER;
      goto error;
    }

    status = ml_single_get_output_info (single_h, &out_info);
    if (status != ML_ERROR_NONE) {
      ml_loge ("Failed to get the output tensor info.");
      goto error;
    }

    status = ml_tensors_info_clone (&single_h->out_info, out_info);
    ml_tensors_info_destroy (out_info);
    if (status != ML_ERROR_NONE)
      goto error;

    if (!ml_tensors_info_is_valid (&single_h->out_info, valid)) {
      ml_loge ("The output tensor info is invalid.");
      status = ML_ERROR_INVALID_PARAMETER;
      goto error;
    }
  }

  g_mutex_init (&single_h->mutex);
  g_cond_init (&single_h->cond);
  single_h->state = IDLE;
  single_h->ignore_output = FALSE;

  single_h->thread = g_thread_try_new (NULL, invoke_thread, (gpointer) single_h,
      &error);
  if (single_h->thread == NULL) {
    ml_loge ("Failed to create the invoke thread, error: %s.", error->message);
    g_clear_error (&error);
    status = ML_ERROR_UNKNOWN;
    goto error;
  }

  *single = single_h;
  return ML_ERROR_NONE;

error:
  ml_single_close (single_h);
  return status;
}

/**
 * @brief Closes the opened model handle.
 */
int
ml_single_close (ml_single_h single)
{
  ml_single *single_h;

  check_feature_state ();

  if (!single) {
    ml_loge ("The given param, single is invalid.");
    return ML_ERROR_INVALID_PARAMETER;
  }

  ML_SINGLE_GET_VALID_HANDLE_LOCKED (single_h, single, 1);

  single_h->state = JOIN_REQUESTED;
  g_cond_broadcast (&single_h->cond);

  ML_SINGLE_HANDLE_UNLOCK (single_h);
  g_thread_join (single_h->thread);

  /** locking ensures correctness with parallel calls on close */
  if (single_h->filter) {
    GTensorFilterSingleClass *klass;
    klass = g_type_class_peek (G_TYPE_TENSOR_FILTER_SINGLE);
    if (klass)
      klass->stop (single_h->filter);
    gst_object_unref (single_h->filter);
    single_h->filter = NULL;
  }

  ml_tensors_info_free (&single_h->in_info);
  ml_tensors_info_free (&single_h->out_info);

  g_cond_clear (&single_h->cond);
  g_mutex_clear (&single_h->mutex);

  g_free (single_h);
  return ML_ERROR_NONE;
}

/**
 * @brief Invokes the model with the given input data.
 */
int
ml_single_invoke (ml_single_h single,
    const ml_tensors_data_h input, ml_tensors_data_h * output)
{
  ml_single *single_h;
  ml_tensors_data_s *in_data;
  gint64 end_time;
  int i, status = ML_ERROR_NONE;

  check_feature_state ();

  if (!single || !input || !output) {
    ml_loge ("The given param is invalid.");
    return ML_ERROR_INVALID_PARAMETER;
  }

  ML_SINGLE_GET_VALID_HANDLE_LOCKED (single_h, single, 0);

  in_data = (ml_tensors_data_s *) input;
  *output = NULL;

  if (!single_h->filter || single_h->state >= JOIN_REQUESTED) {
    ml_loge ("The given param is invalid, model is missing.");
    status = ML_ERROR_INVALID_PARAMETER;
    goto exit;
  }

  /* Validate input data */
  if (in_data->num_tensors != single_h->in_info.num_tensors) {
    ml_loge ("The given param input is invalid, \
        different number of memory blocks.");
    status = ML_ERROR_INVALID_PARAMETER;
    goto exit;
  }

  for (i = 0; i < in_data->num_tensors; i++) {
    size_t raw_size = ml_tensor_info_get_size (&single_h->in_info.info[i]);

    if (!in_data->tensors[i].tensor || in_data->tensors[i].size != raw_size) {
      ml_loge ("The given param input is invalid, \
          different size of memory block.");
      status = ML_ERROR_INVALID_PARAMETER;
      goto exit;
    }
  }

  if (single_h->state != IDLE) {
    status = ML_ERROR_TRY_AGAIN;
    goto exit;
  }

  single_h->input = input;
  single_h->output = output;
  single_h->state = RUNNING;
  single_h->ignore_output = FALSE;

  end_time = g_get_monotonic_time () +
    single_h->timeout * G_TIME_SPAN_MILLISECOND;

  g_cond_broadcast (&single_h->cond);
  if (g_cond_wait_until (&single_h->cond, &single_h->mutex, end_time)) {
    status = single_h->status;
  } else {
    ml_logw ("Wait for invoke has timed out");
    status = ML_ERROR_TIMED_OUT;
    /** This is set to notify invoke_thread to not process if timedout */
    single_h->ignore_output = TRUE;

    /** Free if any output memory was allocated */
    if (*single_h->output != NULL) {
      ml_tensors_data_destroy ((ml_tensors_data_h) *single_h->output);
      *single_h->output = NULL;
    }
  }

exit:
  ML_SINGLE_HANDLE_UNLOCK (single_h);
  return status;
}

/**
 * @brief Gets the type of required input data for the given handle.
 * @note type = (tensor dimension, type, name and so on)
 */
int
ml_single_get_input_info (ml_single_h single, ml_tensors_info_h * info)
{
  ml_single *single_h;
  ml_tensors_info_s *input_info;
  GstTensorsInfo gst_info;
  gchar *val;
  guint rank;

  check_feature_state ();

  if (!single || !info)
    return ML_ERROR_INVALID_PARAMETER;

  ML_SINGLE_GET_VALID_HANDLE_LOCKED (single_h, single, 0);

  /* allocate handle for tensors info */
  ml_tensors_info_create (info);
  input_info = (ml_tensors_info_s *) (*info);

  gst_tensors_info_init (&gst_info);

  g_object_get (single_h->filter, "input", &val, NULL);
  rank = gst_tensors_info_parse_dimensions_string (&gst_info, val);
  g_free (val);

  /* set the number of tensors */
  gst_info.num_tensors = rank;

  g_object_get (single_h->filter, "inputtype", &val, NULL);
  rank = gst_tensors_info_parse_types_string (&gst_info, val);
  g_free (val);

  if (gst_info.num_tensors != rank) {
    ml_logw ("Invalid state, input tensor type is mismatched in filter.");
  }

  g_object_get (single_h->filter, "inputname", &val, NULL);
  rank = gst_tensors_info_parse_names_string (&gst_info, val);
  g_free (val);

  if (gst_info.num_tensors != rank) {
    ml_logw ("Invalid state, input tensor name is mismatched in filter.");
  }

  ML_SINGLE_HANDLE_UNLOCK (single_h);

  ml_tensors_info_copy_from_gst (input_info, &gst_info);
  gst_tensors_info_free (&gst_info);
  return ML_ERROR_NONE;
}

/**
 * @brief Gets the type of required output data for the given handle.
 * @note type = (tensor dimension, type, name and so on)
 */
int
ml_single_get_output_info (ml_single_h single, ml_tensors_info_h * info)
{
  ml_single *single_h;
  ml_tensors_info_s *output_info;
  GstTensorsInfo gst_info;
  gchar *val;
  guint rank;

  check_feature_state ();

  if (!single || !info)
    return ML_ERROR_INVALID_PARAMETER;

  ML_SINGLE_GET_VALID_HANDLE_LOCKED (single_h, single, 0);

  /* allocate handle for tensors info */
  ml_tensors_info_create (info);
  output_info = (ml_tensors_info_s *) (*info);

  gst_tensors_info_init (&gst_info);

  g_object_get (single_h->filter, "output", &val, NULL);
  rank = gst_tensors_info_parse_dimensions_string (&gst_info, val);
  g_free (val);

  /* set the number of tensors */
  gst_info.num_tensors = rank;

  g_object_get (single_h->filter, "outputtype", &val, NULL);
  rank = gst_tensors_info_parse_types_string (&gst_info, val);
  g_free (val);

  if (gst_info.num_tensors != rank) {
    ml_logw ("Invalid state, output tensor type is mismatched in filter.");
  }

  g_object_get (single_h->filter, "outputname", &val, NULL);
  rank = gst_tensors_info_parse_names_string (&gst_info, val);
  g_free (val);

  if (gst_info.num_tensors != rank) {
    ml_logw ("Invalid state, output tensor name is mismatched in filter.");
  }

  ML_SINGLE_HANDLE_UNLOCK (single_h);

  ml_tensors_info_copy_from_gst (output_info, &gst_info);
  gst_tensors_info_free (&gst_info);
  return ML_ERROR_NONE;
}

/**
 * @brief Sets the maximum amount of time to wait for an output, in milliseconds.
 */
int
ml_single_set_timeout (ml_single_h single, unsigned int timeout)
{
  ml_single *single_h;

  check_feature_state ();

  if (!single || timeout == 0)
    return ML_ERROR_INVALID_PARAMETER;

  ML_SINGLE_GET_VALID_HANDLE_LOCKED (single_h, single, 0);

  single_h->timeout = (guint) timeout;

  ML_SINGLE_HANDLE_UNLOCK (single_h);
  return ML_ERROR_NONE;
}
