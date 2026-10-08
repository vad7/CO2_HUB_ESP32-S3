/* Общий код страниц. Прошивка отдаёт страницы как есть, данные — отдельно:
     GET  /api/vars?cfg_fan_=N&g=main,fans — переменные одним JSON-объектом: только группы из
                                  <body data-groups="main,fans"> (+ sys — всегда: подвал, период опроса);
                                  без data-groups — все группы. Группы и их переменные — web.cpp (VAR_GROUPS);
     POST /api/set              — запись, тело application/x-www-form-urlencoded в UTF-8
                                  (URLSearchParams сам кодирует русский текст и спецсимволы: %D0%9A..., '+' -> %2B).
   Разметка страниц:
     <div id="app_menu"></div>, <div id="app_footer"></div> — сюда app.js вставляет меню (со строкой
                                      состояния) и подвал — общие для всех страниц (App.MENU);
     data-var="имя"                 — текст элемента = значение переменной (обновляется при каждом опросе);
     data-var="имя" data-fmt="time" — UTC-секунды -> местное время браузера ('---', если 0);
     <input|select name="имя">      — поле формы заполняется значением переменной с тем же именем
                                      (только при загрузке и после сохранения, не при опросе; hidden — не трогаются);
     <form data-api>                — отправка в /api/set без перезагрузки; data-confirm="текст" — с подтверждением;
     data-maxbytes="N"              — предел длины в байтах UTF-8 (кириллица — 2 байта на букву). */
'use strict';

var App = {
	FAN_PARAM: 'cfg_fan_',     // выбранный вентилятор (cookie и параметр запроса)
	vars: {},
	/* Пункты меню: адрес, подпись (HTML) */
	MENU: [
		['/', 'Главная'],
		['/history.htm', 'История'],
		['/settings.htm', 'Настройки'],
		['/info.htm', '?']                 /* система, память и буферы; вентиляторы — из «Настроек» */
	],

	/* ---------- общие части страниц ---------- */
	layout: function () {
		var m = document.getElementById('app_menu');
		if (m && !m.firstChild) {
			var here = location.pathname == '/index.htm' ? '/' : location.pathname, h = '<div class="menu">';
			for (var i = 0; i < App.MENU.length; i++)
				h += '<a href="' + App.MENU[i][0] + '"' + (App.MENU[i][0] == here ? ' class="active"' : '') + '>' +
					App.MENU[i][1] + '</a>';
			m.innerHTML = h + '</div><p id="app_status" class="status" hidden></p>';
		}
		var f = document.getElementById('app_footer');
		if (f && !f.firstChild)
			f.innerHTML = '<div class="footer">Wireless CO<sub>2</sub> · <span data-var="sys_board"></span> · ' +
				'<span data-var="sys_sensor"></span> · ' +
				'<span data-var="sys_ver"></span> (<span data-var="sys_build"></span>)</div>';
	},

	/* ---------- cookies ---------- */
	setCookie: function (name, value) {
		document.cookie = name + '=' + encodeURIComponent(value) + '; path=/; max-age=31536000';
	},
	getCookie: function (name) {
		var m = document.cookie.match('(?:^|; )' + name.replace(/([.$?*|{}()\[\]\\\/+^])/g, '\\$1') + '=([^;]*)');
		return m ? decodeURIComponent(m[1]) : null;
	},
	fan: function () {
		return Number(App.getCookie(App.FAN_PARAM)) || 0;
	},
	setFan: function (i) {
		App.setCookie(App.FAN_PARAM, i);
	},

	/* ---------- вывод ---------- */
	timeText: function (sec) {
		var n = Number(sec);
		return n ? new Date(n * 1000).toLocaleString('ru-RU') : '---';
	},
	/* Объём: «512 б», «181 Кб», «2.3 Мб» */
	bytes: function (n) {
		n = Number(n) || 0;
		if (n < 1024) return n + ' б';
		if (n < 1048576) return Math.round(n / 1024) + ' Кб';
		return (n / 1048576).toFixed(1) + ' Мб';
	},
	utf8Len: function (s) {
		return new TextEncoder().encode(s).length;
	},
	/* Строка состояния под меню: ошибки связи, «Сохранено» */
	status: function (text, isError) {
		var e = document.getElementById('app_status');
		if (!e) return;
		e.textContent = text;
		e.className = 'status' + (isError ? ' error' : '');
		e.hidden = !text;
		clearTimeout(App._stTimer);
		if (text && !isError) App._stTimer = setTimeout(function () { e.hidden = true; }, 3000);
	},
	show: function (v) {
		var list = document.querySelectorAll('[data-var]');
		for (var i = 0; i < list.length; i++) {
			var e = list[i], n = e.getAttribute('data-var');
			if (!(n in v)) continue;
			e.textContent = e.getAttribute('data-fmt') == 'time' ? App.timeText(v[n]) : v[n];
		}
	},
	/* root — только поля внутри этого элемента (по умолчанию — вся страница) */
	fillForms: function (v, root) {
		var list = (root || document).querySelectorAll('input[name], select[name], textarea[name]');
		for (var i = 0; i < list.length; i++) {
			var e = list[i];
			if (!(e.name in v) || e.type == 'hidden' || e.type == 'submit' || e.type == 'button') continue;
			var val = String(v[e.name]);
			if (e.type == 'radio') e.checked = e.value == val;
			else if (e.type == 'checkbox') e.checked = Number(val) != 0;
			else e.value = val;
		}
	},

	/* ---------- обмен с устройством ---------- */
	/* Загрузить переменные; fill — заполнить поля форм; done(v) — после успешной загрузки */
	load: function (fill, done) {
		var q = new URLSearchParams(), g = document.body.getAttribute('data-groups');
		q.append(App.FAN_PARAM, App.fan());
		/* g — без URLSearchParams: запятые как есть (прошивка всё равно декодирует %2C) */
		fetch('/api/vars?' + q.toString() + (g ? '&g=' + g : ''), { cache: 'no-store' })
			.then(function (r) {
				if (!r.ok) throw new Error('HTTP ' + r.status);
				return r.json();
			})
			.then(function (v) {
				App.vars = v;
				if (v.json_overflow) {   /* группа не влезла в буфер прошивки (WEB_JSON_MAX) */
					App.status('Прошивка: группа «' + v.json_overflow + '» не помещается в WEB_JSON_MAX', true);
					App._lostLink = true;
				} else if (App._lostLink) { App._lostLink = false; App.status(''); }
				App.show(v);
				if (fill) App.fillForms(v);
				if (done) done(v);
			})
			.catch(function (e) {
				App._lostLink = true;
				App.status('Нет связи с устройством (' + e.message + ')', true);
			});
	},
	/* Опрос каждые ms (0 — не опрашивать): обновляются data-var, затем each(v) */
	poll: function (ms, each) {
		if (!(ms > 0)) return;
		clearTimeout(App._pollTimer);
		App._pollTimer = setTimeout(function () {
			App.load(false, function (v) {
				if (each) each(v);
			});
			App.poll(ms, each);
		}, ms);
	},
	/* Запись переменных: params — объект {имя: значение}, URLSearchParams или FormData.
	   Выбранный вентилятор подставляется первым (поля cfg_fan_* относятся к нему), если не указан явно. */
	send: function (params, done) {
		var p = new URLSearchParams(params), body = new URLSearchParams();
		if (!p.has(App.FAN_PARAM)) body.append(App.FAN_PARAM, App.fan());
		p.forEach(function (val, key) { body.append(key, val); });
		fetch('/api/set', { method: 'POST', body: body })
			.then(function (r) {
				if (!r.ok) throw new Error('HTTP ' + r.status);
				return r.json();
			})
			.then(function (res) {
				if (res.unknown) App.status('Устройство не знает параметр «' + res.unknown + '»', true);
				else App.status(res.restart ? 'Перезапуск устройства...' : 'Сохранено');
				if (done) done(res);
			})
			.catch(function (e) { App.status('Ошибка отправки: ' + e.message, true); });
	},
	/* Проверка data-maxbytes перед отправкой формы */
	checkForm: function (form) {
		var list = form.querySelectorAll('[data-maxbytes]');
		for (var i = 0; i < list.length; i++) {
			var e = list[i], max = Number(e.getAttribute('data-maxbytes')), len = App.utf8Len(e.value);
			if (len > max) {
				App.status('Слишком длинно: ' + len + ' байт при пределе ' + max +
					' (русская буква — 2 байта)', true);
				e.focus();
				return false;
			}
		}
		return true;
	},
	/* Формы data-api: отправка без перезагрузки, затем перечитать значения (прошивка их проверяет и ограничивает).
	   Заполняется заново только отправленная форма — несохранённые правки в других формах страницы остаются. */
	bindForms: function (after) {
		var forms = document.querySelectorAll('form[data-api]');
		for (var i = 0; i < forms.length; i++) {
			forms[i].addEventListener('submit', function (ev) {
				ev.preventDefault();
				var form = ev.target, q = form.getAttribute('data-confirm');
				if (!App.checkForm(form)) return;
				if (q && !confirm(q)) return;
				App.send(new FormData(form), function (res) {
					var pw = form.querySelectorAll('input[type=password]');
					for (var k = 0; k < pw.length; k++) pw[k].value = '';
					if (!res.restart) App.load(false, function (v) {
						App.fillForms(v, form);
						if (after) after(v);
					});
				});
			});
		}
	},
	/* Запуск страницы: формы, первая загрузка с заполнением полей, init(v) */
	start: function (init) {
		App.layout();
		App.bindForms(init);
		App.load(true, init);
	}
};
document.addEventListener('DOMContentLoaded', App.layout);
