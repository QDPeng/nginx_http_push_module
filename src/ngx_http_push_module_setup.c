ngx_module_t  ngx_http_push_module;

static ngx_int_t ngx_http_push_init_module(ngx_cycle_t *cycle) {
	ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
	ngx_http_push_worker_processes = ccf->worker_processes;
	//initialize subscriber queues
	//pool, please
	if((ngx_http_push_pool = ngx_create_pool(NGX_CYCLE_POOL_SIZE, cycle->log))==NULL) { //I trust the cycle pool size to be a well-tuned one.
		return NGX_ERROR; 
	}
	
	//initialize our little IPC
	return ngx_http_push_init_ipc(cycle, ngx_http_push_worker_processes);
}

static ngx_int_t ngx_http_push_init_worker(ngx_cycle_t *cycle) {
	if((ngx_http_push_init_ipc_shm(ngx_http_push_worker_processes))!=NGX_OK) {
		return NGX_ERROR;
	}
	return ngx_http_push_register_worker_message_handler(cycle);
}

// shared memory zone initializer
static ngx_int_t	ngx_http_push_init_shm_zone(ngx_shm_zone_t * shm_zone, void *data) {
	if(data) { /* zone already initialized */
		shm_zone->data = data;
		return NGX_OK;
	}

	ngx_slab_pool_t                *shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
	ngx_rbtree_node_t              *sentinel;
	ngx_http_push_shm_data_t       *d;
	
	if ((d = (ngx_http_push_shm_data_t *)ngx_slab_alloc(shpool, sizeof(*d))) == NULL) { //shm_data plus an array.
		return NGX_ERROR;
	} 
	shm_zone->data = d;
	d->ipc=NULL;
	//initialize rbtree
	if ((sentinel = ngx_slab_alloc(shpool, sizeof(*sentinel)))==NULL) {
		return NGX_ERROR;
	}
	ngx_rbtree_init(&d->tree, sentinel, ngx_http_push_rbtree_insert);
	return NGX_OK;
}

//shared memory
static ngx_str_t	ngx_push_shm_name = ngx_string("push_module"); //shared memory segment name
static ngx_int_t	ngx_http_push_set_up_shm(ngx_conf_t *cf, size_t shm_size) {
	ngx_http_push_shm_zone = ngx_shared_memory_add(cf, &ngx_push_shm_name, shm_size, &ngx_http_push_module);
	if (ngx_http_push_shm_zone == NULL) {
		return NGX_ERROR;
	}
	ngx_http_push_shm_zone->init = ngx_http_push_init_shm_zone;
	ngx_http_push_shm_zone->data = (void *) 1; 
	return NGX_OK;
}

static ngx_int_t	ngx_http_push_postconfig(ngx_conf_t *cf) {
	ngx_http_push_main_conf_t	*conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_push_module);

	//initialize shared memory
	size_t                       shm_size;
	if(conf->shm_size==NGX_CONF_UNSET_SIZE) {
		conf->shm_size=NGX_HTTP_PUSH_DEFAULT_SHM_SIZE;
	}
	shm_size = ngx_align(conf->shm_size, ngx_pagesize);
	if (shm_size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "The push_max_reserved_memory value must be at least %udKiB", (8 * ngx_pagesize) >> 10);
        shm_size = 8 * ngx_pagesize;
    }
	if(ngx_http_push_shm_zone && ngx_http_push_shm_zone->shm.size != shm_size) {
		ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Cannot change memory area size without restart, ignoring change");
	}
	ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "Using %udKiB of shared memory for push module", shm_size >> 10);
	
	return ngx_http_push_set_up_shm(cf, shm_size);
}

//main config
static void * 		ngx_http_push_create_main_conf(ngx_conf_t *cf) {
	ngx_http_push_main_conf_t      *mcf = ngx_pcalloc(cf->pool, sizeof(*mcf));
	if(mcf == NULL) {
		return NGX_CONF_ERROR;
	}
	mcf->shm_size=NGX_CONF_UNSET_SIZE;
	return mcf;
}

//location config stuff
static void *		ngx_http_push_create_loc_conf(ngx_conf_t *cf) {
	ngx_http_push_loc_conf_t       *lcf = ngx_pcalloc(cf->pool, sizeof(*lcf));
	if(lcf == NULL) {
		return NGX_CONF_ERROR;
	}
	lcf->buffer_timeout=NGX_CONF_UNSET;
	lcf->max_messages=NGX_CONF_UNSET;
	lcf->min_messages=NGX_CONF_UNSET;
	lcf->subscriber_concurrency=NGX_CONF_UNSET;
	lcf->subscriber_poll_mechanism=NGX_CONF_UNSET;
	lcf->authorize_channel=NGX_CONF_UNSET;
	lcf->store_messages=NGX_CONF_UNSET;
	lcf->min_message_recipients=NGX_CONF_UNSET;
	lcf->max_channel_id_length=NGX_CONF_UNSET;
	lcf->channel_group.data=NULL;
	return lcf;
}

static char *	ngx_http_push_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
	ngx_http_push_loc_conf_t       *prev = parent, *conf = child;
	ngx_conf_merge_sec_value(conf->buffer_timeout, prev->buffer_timeout, NGX_HTTP_PUSH_DEFAULT_BUFFER_TIMEOUT);
	ngx_conf_merge_value(conf->max_messages, prev->max_messages, NGX_HTTP_PUSH_DEFAULT_MAX_MESSAGES);
	ngx_conf_merge_value(conf->min_messages, prev->min_messages, NGX_HTTP_PUSH_DEFAULT_MIN_MESSAGES);
	ngx_conf_merge_value(conf->subscriber_concurrency, prev->subscriber_concurrency, NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_LASTIN);
	ngx_conf_merge_value(conf->subscriber_poll_mechanism, prev->subscriber_poll_mechanism, NGX_HTTP_PUSH_MECHANISM_LONGPOLL);
	ngx_conf_merge_value(conf->authorize_channel, prev->authorize_channel, 0);
	ngx_conf_merge_value(conf->store_messages, prev->store_messages, 1);
	ngx_conf_merge_value(conf->min_message_recipients, prev->min_message_recipients, NGX_HTTP_PUSH_MIN_MESSAGE_RECIPIENTS);
	ngx_conf_merge_value(conf->max_channel_id_length, prev->max_channel_id_length, NGX_HTTP_PUSH_MAX_CHANNEL_ID_LENGTH);
	ngx_conf_merge_str_value(conf->channel_group, prev->channel_group, "");
	
	//sanity checks
	if(conf->max_messages < conf->min_messages) {
		//min/max buffer size makes sense?
		ngx_conf_log_error(NGX_LOG_ERR, cf, 0, "push_max_message_buffer_length cannot be smaller than push_max_message_buffer_length.");
		return NGX_CONF_ERROR;
	}
	
	return NGX_CONF_OK;
}

static ngx_str_t  ngx_http_push_channel_id = ngx_string("push_channel_id"); //channel id variable
//publisher and subscriber handlers now.
static char *ngx_http_push_setup_handler(ngx_conf_t *cf, void * conf, ngx_int_t (*handler)(ngx_http_request_t *)) {
	ngx_http_core_loc_conf_t       *clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	ngx_http_push_loc_conf_t       *plcf = conf;
	clcf->handler = handler;
	clcf->if_modified_since = NGX_HTTP_IMS_OFF;
	plcf->index = ngx_http_get_variable_index(cf, &ngx_http_push_channel_id);
	if (plcf->index == NGX_ERROR) {
		return NGX_CONF_ERROR;
	}
	return NGX_CONF_OK;
}

static char *ngx_http_push_set_subscriber_concurrency(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	ngx_str_t                      *value=&(((ngx_str_t *) cf->args->elts)[1]);
	ngx_int_t                      *field = (ngx_int_t *) ((char *) conf + cmd->offset);
	if (*field != NGX_CONF_UNSET) {
		return "is duplicate";
	}
	if(ngx_strncmp(value->data, "first", 5)==0) {
		*field=NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_FIRSTIN;
	}
	else if(ngx_strncmp(value->data, "last", 4)==0) {
		*field=NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_LASTIN;
	}
	else if(ngx_strncmp(value->data, "broadcast", 9)==0) {
		*field=NGX_HTTP_PUSH_SUBSCRIBER_CONCURRENCY_BROADCAST;
	}
	else {
		ngx_conf_log_error(NGX_LOG_WARN, cf, 0, "Unexpected value for push_subscriber_concurrency.");
		return NGX_CONF_ERROR;
	}
	if (cmd->post) {
		return ((ngx_conf_post_t *) cmd->post)->post_handler(cf, (ngx_conf_post_t *) cmd->post, field);
	}

	return NGX_CONF_OK;
}

static char *ngx_http_push_publisher(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	return ngx_http_push_setup_handler(cf, conf, &ngx_http_push_publisher_handler);
}

static char *ngx_http_push_subscriber(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	ngx_int_t                      *field = (ngx_int_t *) ((char *) conf + cmd->offset);
	ngx_str_t                      *value = &(((ngx_str_t *) cf->args->elts)[1]);
	
	if (*field != NGX_CONF_UNSET) {
		return "is duplicate";
	}
	if(ngx_strncmp(value->data, "interval-poll", 8)==0) {
		*field=NGX_HTTP_PUSH_MECHANISM_INTERVALPOLL;
	}
	else { // if(ngx_strncmp(value->data, "long-poll", 4)==0)
		*field=NGX_HTTP_PUSH_MECHANISM_LONGPOLL;
	}
	
	return ngx_http_push_setup_handler(cf, conf, &ngx_http_push_subscriber_handler);
}


static ngx_command_t  ngx_http_push_commands[] = {

    { ngx_string("push_message_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_sec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, buffer_timeout),
      NULL },

    { ngx_string("push_max_reserved_memory"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_push_main_conf_t, shm_size),
      NULL },
	  
	{ ngx_string("push_min_message_buffer_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, min_messages),
      NULL },
	
	{ ngx_string("push_max_message_buffer_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, max_messages),
      NULL },
	  
	{ ngx_string("push_min_message_recipients"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, min_message_recipients),
      NULL },

	{ ngx_string("push_publisher"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_push_publisher,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
	
	{ ngx_string("push_subscriber"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS|NGX_CONF_TAKE1,
      ngx_http_push_subscriber,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, subscriber_poll_mechanism),
      NULL },
	
    { ngx_string("push_subscriber_concurrency"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_push_set_subscriber_concurrency,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, subscriber_concurrency),
      NULL },
	  
	{ ngx_string("push_authorized_channels_only"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, authorize_channel),
      NULL },
	  
	{ ngx_string("push_store_messages"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, store_messages),
      NULL },
	  
	{ ngx_string("push_channel_group"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, channel_group),
      NULL },
	  
	{ ngx_string("push_max_channel_id_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, max_channel_id_length),
      NULL },

	  
	//deprecated and misleading. remove no earlier than november 2009.
	{ ngx_string("push_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_push_main_conf_t, shm_size),
      NULL },
	
	//deprecated. remove no earlier than november 2009.
	{ ngx_string("push_queue_messages"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_push_loc_conf_t, store_messages),
      NULL },
    
    ngx_null_command
};

static ngx_http_module_t  ngx_http_push_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_push_postconfig,              /* postconfiguration */
    ngx_http_push_create_main_conf,        /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
    ngx_http_push_create_loc_conf,         /* create location configuration */
    ngx_http_push_merge_loc_conf,          /* merge location configuration */
};

ngx_module_t  ngx_http_push_module = {
    NGX_MODULE_V1,
    &ngx_http_push_module_ctx,             /* module context */
    ngx_http_push_commands,                /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_http_push_init_module,             /* init module */
    ngx_http_push_init_worker,             /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};
