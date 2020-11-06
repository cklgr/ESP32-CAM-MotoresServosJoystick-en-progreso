
#include <esp32-hal-ledc.h>
int speed = 255;  
int noStop = 0;


#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"

#include "dl_lib.h"

typedef struct {
        httpd_req_t *req;
        size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if(!index){
        j->len = 0;
    }
    if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
       // Serial.println("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

    size_t out_len, out_width, out_height;
    uint8_t * out_buf;
    bool s;
    {
        size_t fb_len = 0;
        if(fb->format == PIXFORMAT_JPEG){
            fb_len = fb->len;
            res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        } else {
            jpg_chunking_t jchunk = {req, 0};
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
            httpd_resp_send_chunk(req, NULL, 0);
            fb_len = jchunk.len;
        }
        esp_camera_fb_return(fb);
        int64_t fr_end = esp_timer_get_time();
        // Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
        return res;
    }

    dl_matrix3du_t *image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
    if (!image_matrix) {
        esp_camera_fb_return(fb);
        // Serial.println("dl_matrix3du_alloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    out_buf = image_matrix->item;
    out_len = fb->width * fb->height * 3;
    out_width = fb->width;
    out_height = fb->height;

    s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
    esp_camera_fb_return(fb);
    if(!s){
        dl_matrix3du_free(image_matrix);
        // Serial.println("to rgb888 failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    jpg_chunking_t jchunk = {req, 0};
    s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
    dl_matrix3du_free(image_matrix);
    if(!s){
        // Serial.println("JPEG compression failed");
        return ESP_FAIL;
    }

    int64_t fr_end = esp_timer_get_time();
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];
    dl_matrix3du_t *image_matrix = NULL;

    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            // Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
             {
                if(fb->format != PIXFORMAT_JPEG){
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if(!jpeg_converted){
                        // Serial.println("JPEG compression failed");
                        res = ESP_FAIL;
                    }
                } else {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
            }
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if(_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if(res != ESP_OK){
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        //Serial.printf("MJPG: %uB %ums (%.1ffps)\n",
        //    (uint32_t)(_jpg_buf_len),
        //    (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time           
        //);
    }

    last_frame = 0;
    return res;
}

enum state {fwd,rev,stp};
state actstate = stp;

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    char variable[32] = {0,};
    char value[32] = {0,};

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if(!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
            } else {
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else {
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t * s = esp_camera_sensor_get();
    int res = 0;
    
    if(!strcmp(variable, "framesize")) 
    {
        //Serial.println("framesize");
        if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
    }
    else if(!strcmp(variable, "quality")) 
    {
      //Serial.println("quality");
      res = s->set_quality(s, val);
    }
    //Remote Control Car 
    //Don't use channel 1 and channel 2
    else if(!strcmp(variable, "flash")) 
    {
      ledcWrite(7,val);
    }  
    else if(!strcmp(variable, "speed")) 
    {
      if      (val > 255) val = 255;
      else if (val <   0) val = 0;       
      speed = val;
    }     
    else if(!strcmp(variable, "nostop")) 
    {
      noStop = val;
    }             
    else if(!strcmp(variable, "servo")) // 3250, 4875, 6500
    {
      if      (val > 650) val = 650;
      else if (val < 325) val = 325;       
      ledcWrite(8,10*val);
    }
	else if(!strcmp(variable, "servopan")) // 3250, 4875, 6500
    {
      if      (val > 650) val = 650;
      else if (val < 325) val = 325;       
      ledcWrite(9,10*val);
    }
  else if(!strcmp(variable, "servo3")) // 3250, 4875, 6500
    {
      if      (val > 650) val = 650;
      else if (val < 325) val = 325;       
      ledcWrite(10,10*val);
    }
    else if(!strcmp(variable, "car")) {  
      if (val==1) {
        //Serial.println("Forward");
        actstate = fwd;     
        ledcWrite(4,speed);  // pin 12
        ledcWrite(3,0);      // pin 13
        ledcWrite(5,speed);  // pin 14  
        ledcWrite(6,0);      // pin 15   
        delay(200);
      }
      else if (val==2) {
        //Serial.println("TurnLeft");
        ledcWrite(3,0);
        ledcWrite(5,0); 
        if      (actstate == fwd) { ledcWrite(4,speed); ledcWrite(6,    0); }
        else if (actstate == rev) { ledcWrite(4,    0); ledcWrite(6,speed); }
        else                      { ledcWrite(4,speed); ledcWrite(6,speed); }
        delay(100);              
      }
      else if (val==3) {
        //Serial.println("Stop"); 
        actstate = stp;       
        ledcWrite(4,0);
        ledcWrite(3,0);
        ledcWrite(5,0);     
        ledcWrite(6,0);  
      }
      else if (val==4) {
        //Serial.println("TurnRight");
        ledcWrite(4,0);
        ledcWrite(6,0); 
        if      (actstate == fwd) { ledcWrite(3,    0); ledcWrite(5,speed); }
        else if (actstate == rev) { ledcWrite(3,speed); ledcWrite(5,    0); }
        else                      { ledcWrite(3,speed); ledcWrite(5,speed); }
        delay(100);              
      }
      else if (val==5) {
        //Serial.println("Backward");  
        actstate = rev;      
        ledcWrite(4,0);
        ledcWrite(3,speed);
        ledcWrite(5,0);  
        ledcWrite(6,speed); 
        delay(200);              
      }
      if (noStop!=1) 
      {
        ledcWrite(3, 0);
        ledcWrite(4, 0);  
        ledcWrite(5, 0);  
        ledcWrite(6, 0);
      }         
    }        
    else 
    { 
      //Serial.println("variable");
      res = -1; 
    }

    if(res){ return httpd_resp_send_500(req); }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req){
    static char json_response[1024];

    sensor_t * s = esp_camera_sensor_get();
    char * p = json_response;
    *p++ = '{';

    p+=sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p+=sprintf(p, "\"quality\":%u,", s->status.quality);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!doctype html>
<html>
    <head>
  <title>TABLA</title>


        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width,initial-scale=1">
        <title>ESP32 OV2460</title>
    
    <!-- ESTILOS CSS, HABRA QUE METERLES MANO... -->
        <style>
          body{font-family:Arial,Helvetica,sans-serif;
         background:#181818;
         color:#EFEFEF;
         font-size:16px}    // Tamaño letra fuera botones
         
      h2{font-size:18px}
      section.main{display:flex}
      #menu,section.main{flex-direction:column}
      #menu{display:none;
            flex-wrap:nowrap;
        min-width:340px;
        background:#363636;
        padding:8px;
        border-radius:4px;margin-top:-10px;
        margin-right:10px}
        
      #content{display:flex;flex-wrap:wrap;align-items:stretch}
      
      figure{padding:0;
         margin:0;
         -webkit-margin-before:0;
         margin-block-start:0;
         -webkit-margin-after:0;
         margin-block-end:0;
         -webkit-margin-start:0;
         margin-inline-start:0;
         -webkit-margin-end:0;
         margin-inline-end:0}
         
      figure img{display:block;
           width:100%;
           height:auto;
           border-radius:4px;
           margin-top:8px}
      @media (min-width: 800px) and (orientation:landscape)
      
      {#content{display:flex;
          flex-wrap:nowrap;
          align-items:stretch}
          
      figure img{display:block;
           max-width:100%;
           max-height:calc(100vh - 40px);
           width:auto;
           height:auto}
      figure{padding:0;
         margin:0;
         -webkit-margin-before:0;
         margin-block-start:0;
         -webkit-margin-after:0;
         margin-block-end:0;
         -webkit-margin-start:0;
         margin-inline-start:0;
         -webkit-margin-end:0;
         margin-inline-end:0}}
         
      section#buttons{display:flex;
              flex-wrap:nowrap;
              justify-content:space-between}
      #nav-toggle{cursor:pointer;
            display:block}
      #nav-toggle-cb{outline:0;
             opacity:0;
             width:0;
             height:0}
      #nav-toggle-cb:checked+#menu{display:flex}.input-group{display:flex;
                                 flex-wrap:nowrap;
                                 line-height:22px;
                                 margin:5px 0}
                          .input-group>label{display:inline-block;
                                     padding-right:10px;
                                     min-width:47%}
                          .input-group input,
                          .input-group select{flex-grow:1}
                          .range-max,.range-min{display:inline-block;
                                      padding:0 5px}
                          button{display:block;
                              margin:5px;
                              padding:0 12px;
                              border:0;
                              line-height:28px;
                              cursor:pointer;
                              color:#fff;
                              background:#ff3034;
                              border-radius:5px;
                              font-size:16px;
                              outline:0}
                          button:hover{background:#ff494d}
                          button:active{background:#f21c21}
                          button.disabled{cursor:default;
                                  background:#a0a0a0}
                                  
    /*  COMENTADA TODA LA PARTE DE ESTILOS DE LOS RANGE, SI LOS GIRAS NO FUNCIONAN  */      
                                  
                          /*        
                          input[type=range]{-webkit-appearance:none;
                                    width:100%;
                                    height:22px;
                                    background:#363636;
                                    cursor:pointer;
                                    margin:0}
                          input[type=range]:focus{outline:0}
                          input[type=range]::-webkit-slider-runnable-track{width:100%;
                                                   height:2px;
                                                   cursor:pointer;
                                                   background:#EFEFEF;
                                                   border-radius:0;
                                                   border:0 solid #EFEFEF}
                          input[type=range]::-webkit-slider-thumb{border:1px solid rgba(0,0,30,0);
                                              height:22px;
                                              width:22px;
                                              border-radius:50px;
                                              background:#ff3034;
                                              cursor:pointer;
                                              -webkit-appearance:none;
                                              margin-top:-11.5px}
                          input[type=range]:focus::-webkit-slider-runnable-track{background:#EFEFEF}
                          input[type=range]::-moz-range-track{width:100%;
                                            height:2px;
                                            cursor:pointer;
                                            background:#EFEFEF;
                                            border-radius:0;
                                            border:0 solid #EFEFEF}
                          input[type=range]::-moz-range-thumb{border:1px solid rgba(0,0,30,0);
                                            height:22px;
                                            width:22px;
                                            border-radius:50px;
                                            background:#ff3034;
                                            cursor:pointer}
                          input[type=range]::-ms-track{width:100%;
                                         height:2px;
                                         cursor:pointer;
                                         background:0 0;
                                         border-color:transparent;
                                         color:transparent}
                          input[type=range]::-ms-fill-lower{background:#EFEFEF;
                                            border:0 solid #EFEFEF;
                                            border-radius:0}
                          input[type=range]::-ms-fill-upper{background:#EFEFEF;
                                            border:0 solid #EFEFEF;
                                            border-radius:0}
                          input[type=range]::-ms-thumb{border:1px solid rgba(0,0,30,0);
                                         height:22px;
                                         width:22px;
                                         border-radius:50px;
                                         background:#ff3034;
                                         cursor:pointer;
                                         height:2px}
                          input[type=range]:focus::-ms-fill-lower{background:#EFEFEF}
                          input[type=range]:focus::-ms-fill-upper{background:#363636}
                          */
                          
                          .switch{display:block;
                              position:relative;
                              line-height:22px;
                              font-size:16px;height:22px}
                          .switch input{outline:0;
                                  opacity:0;
                                  width:0;
                                  height:0}
                          .slider{width:50px;
                              height:22px;
                              border-radius:22px;
                              cursor:pointer;
                              background-color:grey}
                          .slider,.slider:before{display:inline-block;
                                       transition:.4s}
                              .slider:before{position:relative;
                                       content:"";
                                       border-radius:50%;
                                       height:16px;
                                       width:16px;
                                       left:4px;
                                       top:3px;
                                       background-color:#fff}
                          input:checked+.slider{background-color:#ff3034}
                          input:checked+.slider:before{-webkit-transform:translateX(26px);
                                         transform:translateX(26px)}
                                         select{border:1px solid #363636;
                                            font-size:14px;
                                            height:22px;
                                            outline:0;
                                            border-radius:5px}
                          .image-container{position:relative;
                                   /*height: 420px;   CAMBIADO*/
                                   min-width:160px;
                                   }          /*ORIG min-width:160px*/
                          .close{position:absolute;
                               right:5px;
                               top:5px;
                               background:#ff3034;
                               min-width:16px;      /*width:640px;          ORIG min-width:16px*/
                               min-width:16px;      /*height:420px;       ORIG min-width:16px*/
                               border-radius:100px;
                               color:#fff;
                               text-align:center;
                               line-height:18px;cursor:pointer}
                          
                          .hidden{
                              visibility: hidden      /*ORIG display:none ASI EL ESPACIO ESTA OCUPADO*/
                              }
                               
                            
                          /* AQUI EMPIEZO A METER COSAS*/
                          input[type=range] {
                                      width: 90%;         /*Ancho del horizontal*/
                                    }

                          .vranger {                                      
                                      transform: rotate(0deg);                                      
                                      -moz-transform: rotate(0deg); /*do same for other browsers if required*/
                                      writing-mode: bt-lr; /* IE */
                                      -webkit-appearance: slider-vertical; /* WebKit */
                                      
                                      /* NADA A PARTIR DE AQUI, SI ROTAS UN RANGE, TE JODES, NO HAY MAS ESTILOS*/
                                      height: 300px;
                                      width: 1px;
                                      /*margin-top: 10px;            Margen por arriba*/
                                    }
                          .img{
                             height: 420px;
                             width: 640px;
                             }
                          
                          /* JOYSTICK */
                          
                          #joy1Div
                          {
                            border: 3px solid #FF0000;
                            height: 200px;
                             width: 200px;
                          }
                          #joy2Div
                          {
                            border: 3px solid #00ff00;
                            height: 200px;
                            width: 200px;
                          }
        </style>
    
    <script>                <!--  Script de los Joystick -->
    /*
 * Name          : joy.js
 * @author       : Roberto D'Amico (Bobboteck)
 * Last modified : 09.06.2020
 * Revision      : 1.1.6
 *
 * Modification History:
 * Date         Version     Modified By    Description
 * 2020-06-09 1.1.6   Roberto D'Amico Fixed Issue #10 and #11
 * 2020-04-20 1.1.5   Roberto D'Amico Correct: Two sticks in a row, thanks to @liamw9534 for the suggestion
 * 2020-04-03               Roberto D'Amico Correct: InternalRadius when change the size of canvas, thanks to @vanslipon for the suggestion
 * 2020-01-07 1.1.4   Roberto D'Amico Close #6 by implementing a new parameter to set the functionality of auto-return to 0 position
 * 2019-11-18 1.1.3   Roberto D'Amico Close #5 correct indication of East direction
 * 2019-11-12   1.1.2       Roberto D'Amico Removed Fix #4 incorrectly introduced and restored operation with touch devices
 * 2019-11-12   1.1.1       Roberto D'Amico Fixed Issue #4 - Now JoyStick work in any position in the page, not only at 0,0
 * 
 * The MIT License (MIT)
 *
 *  This file is part of the JoyStick Project (https://github.com/bobboteck/JoyStick).
 *  Copyright (c) 2015 Roberto D'Amico (Bobboteck).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 
/**
 * @desc Principal object that draw a joystick, you only need to initialize the object and suggest the HTML container
 * @costructor
 * @param container {String} - HTML object that contains the Joystick
 * @param parameters (optional) - object with following keys:
 *  title {String} (optional) - The ID of canvas (Default value is 'joystick')
 *  width {Int} (optional) - The width of canvas, if not specified is setted at width of container object (Default value is the width of container object)
 *  height {Int} (optional) - The height of canvas, if not specified is setted at height of container object (Default value is the height of container object)
 *  internalFillColor {String} (optional) - Internal color of Stick (Default value is '#00AA00')
 *  internalLineWidth {Int} (optional) - Border width of Stick (Default value is 2)
 *  internalStrokeColor {String}(optional) - Border color of Stick (Default value is '#003300')
 *  externalLineWidth {Int} (optional) - External reference circonference width (Default value is 2)
 *  externalStrokeColor {String} (optional) - External reference circonference color (Default value is '#008000')
 *  autoReturnToCenter {Bool} (optional) - Sets the behavior of the stick, whether or not, it should return to zero position when released (Default value is True and return to zero)
 */
var JoyStick = (function(container, parameters)
{
  parameters = parameters || {};
  var title = (typeof parameters.title === "undefined" ? "joystick" : parameters.title),
    width = (typeof parameters.width === "undefined" ? 0 : parameters.width),
    height = (typeof parameters.height === "undefined" ? 0 : parameters.height),
    internalFillColor = (typeof parameters.internalFillColor === "undefined" ? "#00AA00" : parameters.internalFillColor),
    internalLineWidth = (typeof parameters.internalLineWidth === "undefined" ? 2 : parameters.internalLineWidth),
    internalStrokeColor = (typeof parameters.internalStrokeColor === "undefined" ? "#003300" : parameters.internalStrokeColor),
    externalLineWidth = (typeof parameters.externalLineWidth === "undefined" ? 2 : parameters.externalLineWidth),
    externalStrokeColor = (typeof parameters.externalStrokeColor ===  "undefined" ? "#008000" : parameters.externalStrokeColor),
    autoReturnToCenter = (typeof parameters.autoReturnToCenter === "undefined" ? true : parameters.autoReturnToCenter);
  
  // Create Canvas element and add it in the Container object
  var objContainer = document.getElementById(container);
  var canvas = document.createElement("canvas");
  canvas.id = title;
  if(width === 0) { width = objContainer.clientWidth; }
  if(height === 0) { height = objContainer.clientHeight; }
  canvas.width = width;
  canvas.height = height;
  objContainer.appendChild(canvas);
  var context=canvas.getContext("2d");
  
  var pressed = 0; // Bool - 1=Yes - 0=No
    var circumference = 2 * Math.PI;
  /*    ORIGINAL  */
    var internalRadius = (canvas.width-((canvas.width/2)+10))/2;    
  var maxMoveStick = internalRadius + 5;
  var externalRadius = internalRadius + 30;
  
  /*
  var externalRadius = canvas.width - 30;
  var internalRadius = externalRadius /12;    
  var maxMoveStick = internalRadius + 5;
  */
  
  var centerX = canvas.width / 2;
  var centerY = canvas.height / 2;
  var directionHorizontalLimitPos = canvas.width / 10;
  var directionHorizontalLimitNeg = directionHorizontalLimitPos * -1;
  var directionVerticalLimitPos = canvas.height / 10;
  var directionVerticalLimitNeg = directionVerticalLimitPos * -1;
  // Used to save current position of stick
  var movedX=centerX;
  var movedY=centerY;
    
  // Check if the device support the touch or not
  if("ontouchstart" in document.documentElement)
  {
    canvas.addEventListener("touchstart", onTouchStart, false);
    canvas.addEventListener("touchmove", onTouchMove, false);
    canvas.addEventListener("touchend", onTouchEnd, false);
  }
  else
  {
    canvas.addEventListener("mousedown", onMouseDown, false);
    canvas.addEventListener("mousemove", onMouseMove, false);
    canvas.addEventListener("mouseup", onMouseUp, false);
  }
  // Draw the object
  drawExternal();
  drawInternal();

  /******************************************************
   * Private methods
   *****************************************************/

  /**
   * @desc Draw the external circle used as reference position
   */
  function drawExternal()
  {
    context.beginPath();
    context.arc(centerX, centerY, externalRadius, 0, circumference, false);
    context.lineWidth = externalLineWidth;
    context.strokeStyle = externalStrokeColor;
    context.stroke();
  }

  /**
   * @desc Draw the internal stick in the current position the user have moved it
   */
  function drawInternal()
  {
    context.beginPath();
    if(movedX<internalRadius) { movedX=maxMoveStick; }
    if((movedX+internalRadius) > canvas.width) { movedX = canvas.width-(maxMoveStick); }
    if(movedY<internalRadius) { movedY=maxMoveStick; }
    if((movedY+internalRadius) > canvas.height) { movedY = canvas.height-(maxMoveStick); }
    context.arc(movedX, movedY, internalRadius, 0, circumference, false);
    // create radial gradient
    var grd = context.createRadialGradient(centerX, centerY, 5, centerX, centerY, 200);
    // Light color
    grd.addColorStop(0, internalFillColor);
    // Dark color
    grd.addColorStop(1, internalStrokeColor);
    context.fillStyle = grd;
    context.fill();
    context.lineWidth = internalLineWidth;
    context.strokeStyle = internalStrokeColor;
    context.stroke();
  }
  
  /**
   * @desc Events for manage touch
   */
  function onTouchStart(event) 
  {
    pressed = 1;
  }

  function onTouchMove(event)
  {
    // Prevent the browser from doing its default thing (scroll, zoom)
    event.preventDefault();
    if(pressed === 1 && event.targetTouches[0].target === canvas)
    {
      movedX = event.targetTouches[0].pageX;
      movedY = event.targetTouches[0].pageY;
      // Manage offset
      if(canvas.offsetParent.tagName.toUpperCase() === "BODY")
      {
        movedX -= canvas.offsetLeft;
        movedY -= canvas.offsetTop;
      }
      else
      {
        movedX -= canvas.offsetParent.offsetLeft;
        movedY -= canvas.offsetParent.offsetTop;
      }
      // Delete canvas
      context.clearRect(0, 0, canvas.width, canvas.height);
      // Redraw object
      drawExternal();
      drawInternal();
    }
  } 

  function onTouchEnd(event) 
  {
    pressed = 0;
    // If required reset position store variable
    if(autoReturnToCenter)
    {
      movedX = centerX;
      movedY = centerY;
    }
    // Delete canvas
    context.clearRect(0, 0, canvas.width, canvas.height);
    // Redraw object
    drawExternal();
    drawInternal();
    //canvas.unbind('touchmove');
  }

  /**
   * @desc Events for manage mouse
   */
  function onMouseDown(event) 
  {
    pressed = 1;
  }

  function onMouseMove(event) 
  {
    if(pressed === 1)
    {
      movedX = event.pageX;
      movedY = event.pageY;
      // Manage offset
      if(canvas.offsetParent.tagName.toUpperCase() === "BODY")
      {
        movedX -= canvas.offsetLeft;
        movedY -= canvas.offsetTop;
      }
      else
      {
        movedX -= canvas.offsetParent.offsetLeft;
        movedY -= canvas.offsetParent.offsetTop;
      }
      // Delete canvas
      context.clearRect(0, 0, canvas.width, canvas.height);
      // Redraw object
      drawExternal();
      drawInternal();
    }
  }

  function onMouseUp(event) 
  {
    pressed = 0;
    // If required reset position store variable
    if(autoReturnToCenter)
    {
      movedX = centerX;
      movedY = centerY;
    }
    // Delete canvas
    context.clearRect(0, 0, canvas.width, canvas.height);
    // Redraw object
    drawExternal();
    drawInternal();
    //canvas.unbind('mousemove');
  }

  /******************************************************
   * Public methods
   *****************************************************/
  
  /**
   * @desc The width of canvas
   * @return Number of pixel width 
   */
  this.GetWidth = function () 
  {
    return canvas.width;
  };
  
  /**
   * @desc The height of canvas
   * @return Number of pixel height
   */
  this.GetHeight = function () 
  {
    return canvas.height;
  };
  
  /**
   * @desc The X position of the cursor relative to the canvas that contains it and to its dimensions
   * @return Number that indicate relative position
   */
  this.GetPosX = function ()
  {
    return movedX;
  };
  
  /**
   * @desc The Y position of the cursor relative to the canvas that contains it and to its dimensions
   * @return Number that indicate relative position
   */
  this.GetPosY = function ()
  {
    return movedY;
  };
  
  /**
   * @desc Normalizzed value of X move of stick
   * @return Integer from -100 to +100
   */
  this.GetX = function ()
  {
    return (100*((movedX - centerX)/maxMoveStick)).toFixed();
  };

  /**
   * @desc Normalizzed value of Y move of stick
   * @return Integer from -100 to +100
   */
  this.GetY = function ()
  {
    return ((100*((movedY - centerY)/maxMoveStick))*-1).toFixed();
  };
  
  /**
   * @desc Get the direction of the cursor as a string that indicates the cardinal points where this is oriented
   * @return String of cardinal point N, NE, E, SE, S, SW, W, NW and C when it is placed in the center
   */
  this.GetDir = function()
  {
    var result = "";
    var orizontal = movedX - centerX;
    var vertical = movedY - centerY;
    
    if(vertical >= directionVerticalLimitNeg && vertical <= directionVerticalLimitPos)
    {
      result = "C";
    }
    if(vertical < directionVerticalLimitNeg)
    {
      result = "N";
    }
    if(vertical > directionVerticalLimitPos)
    {
      result = "S";
    }
    
    if(orizontal < directionHorizontalLimitNeg)
    {
      if(result === "C")
      { 
        result = "W";
      }
      else
      {
        result += "W";
      }
    }
    if(orizontal > directionHorizontalLimitPos)
    {
      if(result === "C")
      { 
        result = "E";
      }
      else
      {
        result += "E";
      }
    }
    
    return result;
  };
  
  
  this.GetSpeed = function()
  {
    var speed = "";
    var speed = Math.round(100 * Math.sqrt(Math.pow(movedX - centerX, 2) + Math.pow(movedY - centerY, 2)) / maxMoveStick);
    /*internal da 111, external da 77.., maxMoveStick da 101-111*/
    if(speed > 100 )
    {
      speed = 100;
    }
    return speed;
    
  }
});

    </script>    <!--  Script de los Joystick -->
  </head>
    <body>
                      <!-- SIII, EMPIEZO CON UNA TABLA, CUANDO CONSIGA ACERTAR CON EL CSS YA QUEDARA MEJOR... -->
  <table  align="center" border ="1">
  <tr>
                    <td colspan="3" style="width:30%" align="center">
                    <button id="get-still">Get Still</button>
                    </td>
                    
                    <td rowspan="6" style="width:50%">
                      <div id="stream-container" class="image-container hidden">
                      <div class="close" id="close-stream">×</div>
                      <img id="stream" src="">
                      </div>
                    </td>
                    
                    
                    
                    <td rowspan="6" style="width:3%" align="center">
                            <input type="range" class="vranger" id="servo" min="200" max="900" value="550" 
                            onchange="try{fetch(document.location.origin+'/control?var=servo&val='+this.value);}catch(e){}">
                    </td>
                    
                    <td colspan="2" style="width:17%" align="center">
                    <button id="toggle-stream">Start Stream</button>
                    </td>
  </tr>
  
                    <td><input type="checkbox" id="nostop" onclick="var noStop=0;if (this.checked) noStop=1;
                                    fetch(document.location.origin+'/control?var=nostop&val='+noStop);">No Stop
                    </td>
                    <td align="center"><button id="forward" onclick="fetch(document.location.origin+'/control?var=car&val=1');">Forward</button>
                    </td>
                    <td></td>
  
          
  
                    <td style="width:6%">Flash</td>
                    <td style="width:11%; height:5%" align="center">
                    <input type="range" id="flash" min="0" max="255" value="0" 
                    onchange="try{fetch(document.location.origin+'/control?var=flash&val='+this.value);}catch(e){}">
                    </td>
  </tr>
  
  <tr>
                    <td style="width:10%; height:5%" align="center"><button id="turnleft" onclick="fetch(document.location.origin+'/control?var=car&val=2');">TurnLeft</button>
                    </td>
                    <td style="width:10%; height:5%" align="center"><button id="stop" onclick="fetch(document.location.origin+'/control?var=car&val=3');">Stop</button>
                    </td>
                    <td style="width:10%; height:5%" align="center"><button id="turnright" onclick="fetch(document.location.origin+'/control?var=car&val=4');">TurnRight</button>
                    </td>
  
                    <td style="width:6%; height:5%">Speed</td>
                    <td style="width:10%; height:5%" align="center">
                    <input type="range" id="speed" min="0" max="255" value="255" 
                    onchange="try{fetch(document.location.origin+'/control?var=speed&val='+this.value);}catch(e){}">
                    </td>
  </tr>
  
  <tr>
  <td></td>
                    <td style="width:10%; height:5%" align="center"><button id="backward" onclick="fetch(document.location.origin+'/control?var=car&val=5');">Backward</button>
                    </td>
  <td></td>
                    <td style="width:6%; height:5%">QUALITY</td>
                    <td style="width:10%; height:5%"align="center"><input type="range" id="quality" min="10" max="63" value="10" 
                    onchange="try{fetch(document.location.origin+'/control?var=quality&val='+this.value);}catch(e){}">
                    </td>
  </tr>
  
  <tr>
  <td colspan="3"><p id="demo"></p> </td>
                    <td style="width:6%; height:5%">Resolution</td>
                    <td style="width:10%; height:5%" align="center"><input type="range" id="framesize" min="0" max="6" value="5" 
                    onchange="try{fetch(document.location.origin+'/control?var=framesize&val='+this.value);}catch(e){}">
                    </td>
  </tr>
  
  <tr>
  <td colspan="3" rowspan="3"><div id="joy1Div"></div>
                                  Pos X:<input id="joy1PosizioneX" type="text" /><br />
                                  Pos Y:<input id="joy1PosizioneY" type="text" /><br />
                                  Dir:<input id="joy1Direzione" type="text" /><br />
                                  X :<input id="joy1X" type="text" /></br>
                                  Y :<input id="joy1Y" type="text" /></br>
                                  
                                  S :<input id="joy1Speed" type="text" />
                                            </td>
  <td colspan="2" rowspan="3"><div id="joy2Div"></div>
  
                                  Pos X:<input id="joy2PosizioneX" type="text" /></br>
                                  Pos Y:<input id="joy2PosizioneY" type="text" /></br>
                                  Dir:<input id="joy2Direzione" type="text" /></br>
                                  X :<input id="joy2X" type="text" /></br>
                                  Y :<input id="joy2Y" type="text" /></br>
        
                                  S :<input id="joy2Speed" type="text" />
                                            </td>
  </tr>
  
  <tr>
  
                    <td align="center"><input type="range" id="servopan" min="200" max="800" value="500" 
                    onchange="try{fetch(document.location.origin+'/control?var=servopan&val='+this.value);}catch(e){}">
                    </td>
                    <td rowspan="2"></td>
  
  </tr>
  
  <tr>
  
                    <td align="center"><input type="range" id="servo3" min="200" max="800" value="500" 
                    onchange="try{fetch(document.location.origin+'/control?var=servo3&val='+this.value);}catch(e){}">
                    </td>
  
  </tr>
  </table>
  
          <script type="text/javascript">         <!-- VARIABLES JOYSTICKS  -->
          // Create JoyStick object into the DIV 'joy1Div'
          var Joy1 = new JoyStick('joy1Div');

          var joy1IinputPosX = document.getElementById("joy1PosizioneX");
          var joy1InputPosY = document.getElementById("joy1PosizioneY");
          var joy1Direzione = document.getElementById("joy1Direzione");
          var joy1X = document.getElementById("joy1X");
          var joy1Y = document.getElementById("joy1Y");

          var joy1Speed = document.getElementById("joy1Speed");             /*SPEED*/

          setInterval(function(){ joy1IinputPosX.value=Joy1.GetPosX(); }, 50);
          setInterval(function(){ joy1InputPosY.value=Joy1.GetPosY(); }, 50);
          setInterval(function(){ joy1Direzione.value=Joy1.GetDir(); }, 50);
          setInterval(function(){ joy1X.value=Joy1.GetX(); }, 50);
          setInterval(function(){ joy1Y.value=Joy1.GetY(); }, 50);

          setInterval(function(){ joy1Speed.value=Joy1.GetSpeed(); }, 50);    /*SPEED*/

          // Create JoyStick object into the DIV 'joy2Div'
          var joy2Param = { "title": "joystick2", "autoReturnToCenter": false };
          var Joy2 = new JoyStick('joy2Div', joy2Param);

          var joy2IinputPosX = document.getElementById("joy2PosizioneX");
          var joy2InputPosY = document.getElementById("joy2PosizioneY");
          var joy2Direzione = document.getElementById("joy2Direzione");
          var joy2X = document.getElementById("joy2X");
          var joy2Y = document.getElementById("joy2Y");
          
          var joy2Speed = document.getElementById("joy2Speed");             /*SPEED*/

          setInterval(function(){ joy2IinputPosX.value=Joy2.GetPosX(); }, 50);
          setInterval(function(){ joy2InputPosY.value=Joy2.GetPosY(); }, 50);
          setInterval(function(){ joy2Direzione.value=Joy2.GetDir(); }, 50);
          setInterval(function(){ joy2X.value=Joy2.GetX(); }, 50);
          setInterval(function(){ joy2Y.value=Joy2.GetY(); }, 50);
          
          setInterval(function(){ joy2Speed.value=Joy2.GetSpeed(); }, 50);    /*SPEED*/
          
          </script> 


  
  <script>              <!-- ESTE ES EL ORIGINAL, AUN NO SE POR DONDE METERLE MANO -->
          document.addEventListener('DOMContentLoaded',
      
      function()
          {function b(B)
          {let C;switch(B.type)
          {case'checkbox':C=B.checked?1:0;
           break;
           case'range':case'select-one':C=B.value;
           break;
           case'button':case'submit':C='1';
           break;
           default:return;}
           
           const D=`${c}/control?var=${B.id}&val=${C}`;
           fetch(D).then(E=>{console.log(`request to ${D} finished, status: ${E.status}`)})
           }
           
           var c=document.location.origin;
           const e=B=>{B.classList.add('hidden')},
           f=B=>{B.classList.remove('hidden')},
           g=B=>{B.classList.add('disabled'),
           B.disabled=!0},
           h=B=>{B.classList.remove('disabled'),
           B.disabled=!1},
           i=(B,C,D)=>{D=!(null!=D)||D;let E;
           'checkbox'===B.type?(E=B.checked,
                      C=!!C,B.checked=C):
                      
                     (E=B.value,B.value=C),
                      D&&E!==C?b(B):
                      
                      !D&&('aec'===B.id?C?e(v):                     
                      f(v):'agc'===B.id?C?(f(t),
                      e(s)):
                      (e(t),f(s)):
                      'awb_gain'===B.id?C?f(x):
                      e(x):
                      'face_recognize'===B.id&&(C?h(n):
                      g(n)))};
                      document.querySelectorAll('.close').forEach(B=>{B.onclick=()=>{e(B.parentNode)}}),
                      fetch(`${c}/status`).then(function(B){return B.json()}).then(function(B){document.querySelectorAll('.default-action').forEach(C=>{i(C,B[C.id],!1)})});
                      
                      const j=document.getElementById('stream'),
                        k=document.getElementById('stream-container'),
                        l=document.getElementById('get-still'),
                        m=document.getElementById('toggle-stream'),
                        n=document.getElementById('face_enroll'),
                        o=document.getElementById('close-stream'),
                        
                        p=()=>{window.stop(),m.innerHTML='Start Stream'},
                        
                        q=()=>{j.src=`${c+':81'}/stream`,
                        f(k),m.innerHTML='Stop Stream'};
                        
                        l.onclick=()=>{p(),
                        j.src=`${c}/capture?_cb=${Date.now()}`,
                        f(k)},
                        o.onclick=()=>{p(),
                        e(k)},
                        m.onclick=()=>{const B='Stops Stream'===m.innerHTML;
                        B?p():q()},
                        n.onclick=()=>{b(n)},
                        document.querySelectorAll('.default-action').forEach(B=>{B.onchange=()=>b(B)});
                        const r=document.getElementById('agc'),
                        s=document.getElementById('agc_gain-group'),
                        t=document.getElementById('gainceiling-group');
                        r.onchange=()=>{b(r),
                        r.checked?(f(t),e(s)):(e(t),f(s))};
                        const u=document.getElementById('aec'),
                        v=document.getElementById('aec_value-group');
                        u.onchange=()=>{b(u),
                        u.checked?e(v):f(v)};
                        const w=document.getElementById('awb_gain'),
                        x=document.getElementById('wb_mode-group');
                        w.onchange=()=>{b(w),
                        w.checked?f(x):e(x)};
                        const y=document.getElementById('face_detect'),
                        z=document.getElementById('face_recognize'),
                        A=document.getElementById('framesize');
                        A.onchange=()=>{b(A),
                        5<A.value&&(i(y,!1),
                        i(z,!1))},
                        y.onchange=()=>{return 5<A.value?(alert('Please select CIF or lower resolution before enabling this feature!'),
                        void i(y,!1)):void(b(y),!y.checked&&(g(n),i(z,!1)))},
                        z.onchange=()=>{return 5<A.value?(alert('Please select CIF or lower resolution before enabling this feature!'),
                        void i(z,!1)):void(b(z),z.checked?(h(n),i(y,!0)):g(n))}});
        </script>
  </body>
</html>
  
  	
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t status_uri = {
        .uri       = "/status",
        .method    = HTTP_GET,
        .handler   = status_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t cmd_uri = {
        .uri       = "/control",
        .method    = HTTP_GET,
        .handler   = cmd_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

   httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };
    
    //Serial.printf("Starting web server on port: '%d'\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &status_uri);
        httpd_register_uri_handler(camera_httpd, &capture_uri);
    }

    config.server_port += 1;
    config.ctrl_port += 1;
    //Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}
