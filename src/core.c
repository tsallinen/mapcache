/*
 *  Copyright 2010 Thomas Bonfort
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <apr_strings.h>
#include "geocache.h"


geocache_http_response *geocache_http_response_create(apr_pool_t *pool) {
   geocache_http_response *response = (geocache_http_response*) apr_pcalloc(pool,
         sizeof(geocache_http_response));
   /* make room for at least Expires, Cache-Control, and Content-Type */ 
   response->headers = apr_table_make(pool,3);
   response->code = 200;
   return response;
}

geocache_http_response *geocache_core_get_tile(geocache_context *ctx, geocache_request_get_tile *req_tile) {
#ifdef DEBUG
   if(req_tile->ntiles ==0) {
      ctx->set_error(ctx,500,"BUG: get_tile called with 0 tiles");
      return NULL;
   }
#endif
   int expires = 0;
   geocache_http_response *response = geocache_http_response_create(ctx->pool);
   int i;
   char *timestr;
   geocache_image *base,*overlay;

   /* this loop retrieves the tiles from the caches, and eventually decodes and merges them together
    * if multiple tiles were asked for */
   for(i=0;i<req_tile->ntiles;i++) {
      geocache_tile *tile = req_tile->tiles[i];
      geocache_tileset_tile_get(ctx, tile);
      if(GC_HAS_ERROR(ctx))
         return NULL;
      if(i==0) {
         response->mtime = tile->mtime;
         expires = tile->expires;
         /* if we have multiple tiles to merge, decode the image data */
         if(req_tile->ntiles>1) {
            base = geocache_imageio_decode(ctx, tile->data);
            if(!base) return NULL;
         }
      } else {
         if(response->mtime < tile->mtime)
            response->mtime = tile->mtime;
         if(tile->expires < expires) {
            expires = tile->expires;
         }
         overlay = geocache_imageio_decode(ctx, tile->data);
         if(!overlay) return NULL;
         geocache_image_merge(ctx, base, overlay);
         if(GC_HAS_ERROR(ctx)) {
            return NULL;
         }
      }
   }
   geocache_image_format *format = NULL;

   /* if we had more than one tile, we need to encode the raw image data into a geocache_buffer */
   if(req_tile->ntiles > 1) {
      if(req_tile->ntiles>1) {
         format = req_tile->tiles[0]->tileset->format;
         if(req_tile->format) {
            format = req_tile->format;
         }
         if(!format) {
            format = ctx->config->default_image_format; /* this one is always defined */
         }
      }
      response->data = format->write(ctx, base, format);
      if(GC_HAS_ERROR(ctx)) {
         return NULL;
      }
   } else {
      response->data = req_tile->tiles[0]->data;
      format = req_tile->tiles[0]->tileset->format;
   }

   /* compute the content-type */
   if(format && format->mime_type) {
      apr_table_set(response->headers,"Content-Type",format->mime_type);
   } else {
      geocache_image_format_type t = geocache_imageio_header_sniff(ctx,response->data);
      if(t == GC_PNG)
         apr_table_set(response->headers,"Content-Type","image/png");
      else if(t == GC_JPEG)
         apr_table_set(response->headers,"Content-Type","image/jpeg");
   }

   /* compute expiry headers */
   if(expires) {
      apr_time_t now = apr_time_now();
      apr_time_t additional = apr_time_from_sec(expires);
      apr_time_t texpires = now + additional;
      apr_table_set(response->headers, "Cache-Control",apr_psprintf(ctx->pool, "max-age=%d", expires));
      timestr = apr_palloc(ctx->pool, APR_RFC822_DATE_LEN);
      apr_rfc822_date(timestr, texpires);
      apr_table_setn(response->headers, "Expires", timestr);
   }

   return response;
}



geocache_image* _core_get_single_map(geocache_context *ctx, geocache_map *map, geocache_resample_mode mode) {

   geocache_tile **maptiles;
   int i,nmaptiles;
   geocache_tileset_get_map_tiles(ctx,map->tileset,map->grid_link,
         map->extent, map->width, map->height,
         &nmaptiles, &maptiles);
   for(i=0;i<nmaptiles;i++) {
      geocache_tile *tile = maptiles[i];
      tile->dimensions = map->dimensions;
      geocache_tileset_tile_get(ctx, tile);
      if(GC_HAS_ERROR(ctx))
         return NULL;
      
      /* update the map modification time if it is older than the tile mtime */
      if(tile->mtime>map->mtime) map->mtime = tile->mtime;

      /* set the map expiration delay to the tile expiration delay,
       * either if the map hasn't got an expiration delay yet
       * or if the tile expiration is shorter than the map expiration
       */
      if(!map->expires || tile->expires<map->expires) map->expires = tile->expires;
   }
   geocache_image *getmapim = geocache_tileset_assemble_map_tiles(ctx,map->tileset,map->grid_link,
         map->extent, map->width, map->height,
         nmaptiles, maptiles,
         mode);
   return getmapim;
}

geocache_http_response *geocache_core_get_map(geocache_context *ctx, geocache_request_get_map *req_map) {
#ifdef DEBUG
   if(req_map->nmaps ==0) {
      ctx->set_error(ctx,500,"BUG: get_map called with 0 maps");
      return NULL;
   }
#endif

   
   if(req_map->getmap_strategy == GEOCACHE_GETMAP_ERROR) {
      ctx->set_error(ctx, 404, "full wms support disabled");
      return NULL;
   }
   
   geocache_image_format *format = NULL;
   geocache_http_response *response = geocache_http_response_create(ctx->pool);
   geocache_map *basemap = req_map->maps[0];

   /* raw image data. if left NULL, there is no need to reencode the image */
   geocache_image *baseim = NULL;

   int i;
   char *timestr;
   if(req_map->getmap_strategy == GEOCACHE_GETMAP_ASSEMBLE) {
      baseim = _core_get_single_map(ctx,basemap,req_map->resample_mode);
      if(GC_HAS_ERROR(ctx)) return NULL;
      for(i=1;i<req_map->nmaps;i++) {
         geocache_map *overlaymap = req_map->maps[i];
         geocache_image *overlayim = _core_get_single_map(ctx,overlaymap,req_map->resample_mode); 
         if(GC_HAS_ERROR(ctx)) return NULL;
         geocache_image_merge(ctx,baseim,overlayim);
         if(GC_HAS_ERROR(ctx)) return NULL;
         if(overlaymap->mtime > basemap->mtime) basemap->mtime = overlaymap->mtime;
         if(!basemap->expires || overlaymap->expires<basemap->expires) basemap->expires = overlaymap->expires;
      }
   } else /*if(ctx->config->getmap_strategy == GEOCACHE_GETMAP_FORWARD)*/ {
      int i;
      for(i=0;i<req_map->nmaps;i++) {
         if(!req_map->maps[i]->tileset->source) {
            ctx->set_error(ctx,404,"cannot forward request for tileset %s: no source configured",
                  req_map->maps[i]->tileset->name);
            return NULL;
         }
      }
      basemap->tileset->source->render_map(ctx, basemap);
      if(GC_HAS_ERROR(ctx)) return NULL;
      if(req_map->nmaps>1) {
         baseim = geocache_imageio_decode(ctx,basemap->data);
         if(GC_HAS_ERROR(ctx)) return NULL;
         for(i=1;i<req_map->nmaps;i++) {
            geocache_map *overlaymap = req_map->maps[i];
            overlaymap->tileset->source->render_map(ctx, overlaymap);
            if(GC_HAS_ERROR(ctx)) return NULL;
            geocache_image *overlayim = geocache_imageio_decode(ctx,overlaymap->data); 
            if(GC_HAS_ERROR(ctx)) return NULL;
            geocache_image_merge(ctx,baseim,overlayim);
            if(GC_HAS_ERROR(ctx)) return NULL;
            if(!basemap->expires || overlaymap->expires<basemap->expires) basemap->expires = overlaymap->expires;
         }
      }
   }
   
   if(baseim) {
      format = req_map->getmap_format; /* always defined, defaults to JPEG */
      response->data = format->write(ctx,baseim,format);
      if(GC_HAS_ERROR(ctx)) {
         return NULL;
      }
   } else {
      /* this case happens when we have a forward strategy for a single tileset */
      response->data = basemap->data;
   }

   /* compute the content-type */
   if(format && format->mime_type) {
      apr_table_set(response->headers,"Content-Type",format->mime_type);
   } else {
      geocache_image_format_type t = geocache_imageio_header_sniff(ctx,response->data);
      if(t == GC_PNG)
         apr_table_set(response->headers,"Content-Type","image/png");
      else if(t == GC_JPEG)
         apr_table_set(response->headers,"Content-Type","image/jpeg");
   }

   /* compute expiry headers */
   if(basemap->expires) {
      apr_time_t now = apr_time_now();
      apr_time_t additional = apr_time_from_sec(basemap->expires);
      apr_time_t texpires = now + additional;
      apr_table_set(response->headers, "Cache-Control",
            apr_psprintf(ctx->pool, "max-age=%d", basemap->expires));
      timestr = apr_palloc(ctx->pool, APR_RFC822_DATE_LEN);
      apr_rfc822_date(timestr, texpires);
      apr_table_setn(response->headers, "Expires", timestr);
   }

   response->mtime = basemap->mtime;
   return response;
}

geocache_http_response *geocache_core_proxy_request(geocache_context *ctx, geocache_request_proxy *req_proxy) {
   geocache_http_response *response = geocache_http_response_create(ctx->pool);
    response->data = geocache_buffer_create(30000,ctx->pool);
    geocache_http *http = req_proxy->http;
    if(req_proxy->pathinfo) {
      http = geocache_http_clone(ctx,http);
      if( (*(req_proxy->pathinfo)) == '/' ||
            http->url[strlen(http->url)-1] == '/')
         http->url = apr_pstrcat(ctx->pool,http->url,req_proxy->pathinfo,NULL);
      else
         http->url = apr_pstrcat(ctx->pool,http->url,"/",req_proxy->pathinfo,NULL);
    }
    geocache_http_do_request_with_params(ctx,http,req_proxy->params,response->data,response->headers,&response->code);
    if(response->code !=0 && GC_HAS_ERROR(ctx)) {
       /* the http request was successful, but the server returned an error */
       ctx->clear_errors(ctx);
    }
    return response;
}

geocache_http_response *geocache_core_get_featureinfo(geocache_context *ctx,
      geocache_request_get_feature_info *req_fi) {
   geocache_feature_info *fi = req_fi->fi;
   geocache_tileset *tileset = fi->map.tileset;
   if(!tileset->source) {
      ctx->set_error(ctx,404,"cannot query tileset %s: no source defined",tileset->name);
      return NULL;
   }
   if(tileset->source->info_formats) {
      int i;
      for(i=0;i<tileset->source->info_formats->nelts;i++) {
         if(!strcmp(fi->format, APR_ARRAY_IDX(tileset->source->info_formats,i,char*))) {
            break;
         }
      }
      if(i == tileset->source->info_formats->nelts) {
         ctx->set_error(ctx,404, "unsupported feature info format %s",fi->format);
         return NULL;
      }
      tileset->source->query_info(ctx,fi);
      if(GC_HAS_ERROR(ctx)) return NULL;
      geocache_http_response *response = geocache_http_response_create(ctx->pool);
      response->data = fi->map.data;
      apr_table_set(response->headers,"Content-Type",fi->format);
      return response;
   } else {
      ctx->set_error(ctx,404, "tileset %s does not support feature info requests");
      return NULL;
   }
}

geocache_http_response* geocache_core_get_capabilities(geocache_context *ctx, geocache_service *service,
      geocache_request_get_capabilities *req_caps, char *url, char *path_info, geocache_cfg *config) {
   service->create_capabilities_response(ctx,req_caps,url,path_info,config);
   geocache_http_response *response = geocache_http_response_create(ctx->pool);
   response->data = geocache_buffer_create(0,ctx->pool);
   response->data->size = strlen(req_caps->capabilities);
   response->data->buf = req_caps->capabilities;
   response->data->avail = response->data->size;
   apr_table_set(response->headers,"Content-Type",req_caps->mime_type);
   return response;
}

geocache_http_response* geocache_core_respond_to_error(geocache_context *ctx, geocache_service *service) {
   //TODO: have the service format the error response
   geocache_http_response *response = geocache_http_response_create(ctx->pool);
   
   /* extract code and message from context */
   response->code = ctx->_errcode;
   if(!response->code) response->code = 500;

   char *msg = ctx->_errmsg;
   if(!msg) {
      msg = "an unspecified error has occured";
   }
   ctx->log(ctx,GEOCACHE_INFO,msg);
  

   if(ctx->config && ctx->config->reporting == GEOCACHE_REPORT_MSG) {
      /* manually populate the geocache_buffer with the error message */
      response->data = geocache_buffer_create(0,ctx->pool);
      response->data->size = strlen(msg);
      response->data->buf = msg;
      response->data->avail = response->data->size;
      apr_table_set(response->headers, "Content-Type", "text/plain");
   } else if(ctx->config && ctx->config->reporting == GEOCACHE_REPORT_EMPTY_IMG) {
      response->data = ctx->config->empty_image;
      apr_table_set(response->headers, "Content-Type", ctx->config->default_image_format->mime_type);
      apr_table_set(response->headers, "X-Geocache-Error", msg);
   } else if(ctx->config && ctx->config->reporting == GEOCACHE_REPORT_ERROR_IMG) {
      geocache_image *errim = geocache_error_image(ctx,256,256,msg);
      geocache_buffer *buf = ctx->config->default_image_format->write(ctx,errim,ctx->config->default_image_format);
      response->data = buf;
      apr_table_set(response->headers, "Content-Type", ctx->config->default_image_format->mime_type);
      apr_table_set(response->headers, "X-Geocache-Error", msg);
   }
   return response;

}

/* vim: ai ts=3 sts=3 et sw=3
*/
