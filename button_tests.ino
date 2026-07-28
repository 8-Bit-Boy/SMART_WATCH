
//pins for push buttons
const int state_button = 9;
const int select_button = 10;



void setup() {
  // put your setup code here, to run once:
Serial.begin(115200);

}

void loop() {
  // put your main code here, to run repeatedly:


// if(digitalRead(state_button)){
//   Serial.println("State button works!!");
// }else if(digitalRead(select_button)){
//   Serial.println("Select button works!!");
// }else{
//   ;
// }


Serial.println(digitalRead(state_button));

}

