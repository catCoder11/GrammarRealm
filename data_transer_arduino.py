import serial
import json
from giga import GigaChatService
from config import TOKEN

# Настройки порта
PORT = 'COM7'
BAUD_RATE = 115200


def listener():
    print("Ожидание подключения к плате...")
    try:
        with serial.Serial(PORT, BAUD_RATE, timeout=3) as ser:

            # Читаем ответ от платы, чтобы убедиться в успехе
            print("\nПодключено. Ожидание ответа от платы...")

            while True:
                incoming_text = ser.readline().decode('utf-8', errors='ignore').strip()
                print(incoming_text)
                if incoming_text == "MESSAGE:GET_QUESTIONS":
                    ser.write("SEND_TEXT\n".encode('utf-8'))
                    ser.flush()
                    loaded_text = ser.readline().decode('utf-8', errors='ignore').strip()
                    # Создаём словарь
                    question_factory = GigaChatService(TOKEN)
                    data_to_send = question_factory.generate_questions(loaded_text)
                    print("Получен текст")
                    print(data_to_send)
                    # Конвертируем словарь в строку формата JSON
                    json_string = json.dumps(data_to_send)
                    print(f"Подготовленная JSON-строка: {json_string}")

                    # Отправляем строку в порт
                    full_payload = f"{json_string}\n"
                    ser.write(full_payload.encode('utf-8'))
                    print("JSON отправлен на плату.")


    except serial.SerialException as e:
        print(f"Ошибка COM-порта: {e}")
        print("Убедитесь, что Монитор порта в Arduino IDE ЗАКРЫТ.")


if __name__ == "__main__":
    listener()
