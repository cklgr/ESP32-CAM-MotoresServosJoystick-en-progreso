 ¡IMPORTANTE!
 
 PARA QUE COMPILE SIN ERRORES EL GESTOR DE TARJETA DEL ESP32 DE ESPRESSIF TIENE QUE SER LA VERSION 1.0.2
 
 Voy tocando cosas desde el anterior

 Basado/Copiado/Ampliado(un poco mas) desde este proyecto
  
  https://github.com/tomasmacek/ESP32CAM_RCTANK
   
  Que a su vez es una modificacion de este otro
  
  https://github.com/PepeTheFroggie/ESP32CAM_RCTANK
  
  
Mis añadidos son pocos pero importantes:

 1º IP Fija, tal como lo he dejado es imprescidible. 
    Lo he cambiado al archivo secrest.h Wifi, contraseña y la IP que prefieras
 
 2º Para añadir un segundo servo (y de paso un tercero) uso  los pines 
    del puerto serie GPIO1 y 3.
    Por lo que he comentado todas las lineas que sacaban 
    informacion al puerto serie.
      
 3º Me quedo solo con tres servos, el cuarto ralentizaba mucho
 
 4º Para intentar cambiar la interface he "tabulado" el archivo app_httpd.cpp 
    tanto la parte HTML como la del SCRIPT. 
    La parte del Script (original del proyecto) no estoy seguro que este bien tabulada, es demasiado compleja para mi.
    
 5º Aqui ya he añadido 2 joystick, de momento solo compruebo que funcionan, tendre que hacer que muevan los motores
    y los servos de giro e inclinacion, el tercero lo dejare en el deslizador (lo mantengo por cabezoneria).
    
    Los joystick estan cogidos de aqui
      https://github.com/bobboteck/JoyStick      
    
    Y les he añadido una variable de velocidad sacada de este otro codigo para joysticks
      https://www.instructables.com/Making-a-Joystick-With-HTML-pure-JavaScript/


PD: No es bonito, esta mal organizado y poco claro, uso incluso una tabla... 
    (ya me pondre con el CSS cuando se me pase el cabreo que tengo con el)
    ¡¡¡PERO FUNCIONA!!!
    


       
      
