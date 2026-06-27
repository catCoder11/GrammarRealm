from gigachat import GigaChat
from gigachat.models import Chat, Messages, MessagesRole
import json
from typing import List
from pydantic import BaseModel, Field
import re


class QuizTemplate(BaseModel):
    questions: List[str] = Field(description="Список вопросов")
    variants: List[str] = Field(description="Варианты ответов, разделенные дефисом. Внутри одной строки варианты ответов только на один вопрос")
    answersText: List[str] = Field(description="Текстовые ответы")
    answers: List[int] = Field(description="Индексы правильных ответов")


class GigaChatService:
    def __init__(self, token: str):
        self.client = GigaChat(
            credentials=token,
            verify_ssl_certs=False
        )

    def _send_prompt(self, prompt: str) -> dict:
        try:
            # Передаем сырую JSON Schema прямо в объект Chat
            response = self.client.chat(
                Chat(
                    messages=[
                        Messages(
                            role=MessagesRole.USER,
                            content=prompt
                        )
                    ],
                    temperature=0.1,
                    response_format={
                        "type": "json_schema",
                        "schema": QuizTemplate.model_json_schema() # Генерирует схему из Pydantic
                    }
                )
            )

            data = response.choices[0].message.content
            print(data)
            try:
                # Очищаем строку от пробелов и переносов
                clean_str = data.strip()

                # Срезаем маркдаун-теги ```json в начале и ``` в конце
                if clean_str.startswith("```json"):
                    clean_str = clean_str[7:]
                elif clean_str.startswith("```"): # на случай, если пришло просто ```
                    clean_str = clean_str[3:]

                if clean_str.endswith("```"):
                    clean_str = clean_str[:-3]

                clean_str = re.sub(r',(\s*[\]\}])', r'\1', clean_str.strip())

                return json.loads(clean_str) # перевод в Python-словарь

            except json.JSONDecodeError as e:
                print(f"Ошибка: нейросеть вернула невалидный JSON. Сырой ответ: {data}")
                return {
                    "questions": ["Ошибка генерации", "Ошибка генерации"],
                    "variants": ["1. - 2. - 3. -", "1. - 2. - 3. -"],
                    "answersText": ["-", "-"],
                    "answers": [1, 1]
                }

        except Exception as e:
            print(f"GigaChat request failed: {e}")
            return {
                "questions": ["Ошибка генерации", "Ошибка генерации", "Ошибка генерации"],
                "variants": ["1. - 2. - 3. -", "1. - 2. - 3. -", "1. - 2. - 3. -"],
                "answersText": ["-", "-", "-"],
                "answers": [1, 1, 1]
            }

    def generate_questions(self, text: str) -> dict:
        prompt = f"""Ты — педагог, составляющий вопросы для проверки понимания текста.
        Составь ровно два вопроса к тексту ниже:
        1. Вопрос на основную мысль или цель текста
        2. Вопрос на конкретную деталь из текста

        Требования:
        - Вопросы должны быть чёткими и однозначными
        - Ответ на каждый вопрос должен содержаться в тексте
        - Выведи только два вопроса, каждый с новой строки
        - Никаких пояснений, нумерации и дополнительного текста
        
        К каждому вопросу дай ровно три варианта ответа, только один из которых правильный. Все варианты для одного варианта в виде одной строки: "1. ... 2. ... 3. ..."
        Отдельно укажи правильный ответ
        Отдельно укажи его номер
        
        ПРАВИЛО ДЛЯ КЛЮЧА "variants":
Этот ключ должен быть СТРОГИМ МАССИВОМ ИЗ ДВУХ ОТДЕЛЬНЫХ СТРОК. Каждая строка — это варианты для своего вопроса.
Запрещено склеивать варианты разных вопросов в одну строку через ";"! Это распространяется и на другие ключи

Пример идеального ответа (соблюдай эту структуру):
{{
  "questions": [
    "Какая ягода большая?", 
    "Кто играл Холмса?"
  ],
  "variants": [
    "1. Арбуз 2. Дыня 3. Тыква", 
    "1. Клюев 2. Петров 3. Харламов"
  ],
  "answersText": [
    "Арбуз", 
    "Клюев"
  ],
  "answers": [1, 1]
}}
помни, ответ нужен в формате настоящего правильного json. Не используй | для разделения
        Текст:
        {text}"""

        return self._send_prompt(prompt)

