# GPC Phase 2 — تشغيل سريع

المتطلبات: Python 3.10+ (لا توجد حزم خارجية).

```bash
# أعد اختبار المرحلة الأولى
python3 gpc_phase1.py

# شغّل الاختبارات والمحرك المصغر وشهادات AND/OR
python3 gpc_phase2.py --self-test

# افحص شهادة 9x7 من دون تشغيل solver
python3 gpc_phase2.py --verify GPC_Phase2_Certificates/9x7_Block_and_Fork.json
python3 gpc_phase2.py --verify GPC_Phase2_Certificates/9x7_Same_Column_Threat_Ladder.json

# حل حالة/تاريخ نقلات صغير، مع إخراج شهادة
python3 gpc_phase2.py --board 4 3 3 --history "" --solve --certificate cert.json
```

ترقيم الأعمدة صفر-يبدأ. ملفا الشهادة للوحة 9×7 يتضمنان تاريخ النقلات من اللوحة الفارغة حتى الحالة التي يثبتان نتيجتها. الشهادات لا تدعي حل الوضع الابتدائي 9×7.
