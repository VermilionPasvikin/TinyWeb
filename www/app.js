// TinyWeb 测试站点 - JavaScript 工具库

async function fetchCGI(url, method = 'GET', body = null) {
  try {
    const options = {
      method: method,
      headers: {}
    };

    if (method === 'POST' && body) {
      options.headers['Content-Type'] = 'application/x-www-form-urlencoded';
      options.body = body;
    }

    const response = await fetch(url, options);
    const contentType = response.headers.get('content-type');

    if (contentType && contentType.includes('application/json')) {
      return await response.json();
    } else {
      return await response.text();
    }
  } catch (error) {
    showError('请求失败: ' + error.message);
    throw error;
  }
}

function displayResponse(elementId, data, isSuccess = true) {
  const element = document.getElementById(elementId);
  if (!element) return;

  element.className = 'response-box ' + (isSuccess ? 'success' : 'error');

  if (typeof data === 'object') {
    element.innerHTML = '<pre>' + JSON.stringify(data, null, 2) + '</pre>';
  } else {
    element.innerHTML = '<div>' + escapeHtml(data) + '</div>';
  }
}

function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

async function submitForm(formElement, cgiUrl, resultElementId) {
  const formData = new FormData(formElement);
  const params = new URLSearchParams(formData);

  try {
    const result = await fetchCGI(cgiUrl, 'POST', params.toString());
    displayResponse(resultElementId, result, true);
  } catch (error) {
    displayResponse(resultElementId, '提交失败: ' + error.message, false);
  }
}

function updateTime() {
  const timeElements = document.querySelectorAll('.current-time');
  const now = new Date().toLocaleString('zh-CN');
  timeElements.forEach(el => el.textContent = now);
}

function showError(message) {
  console.error(message);
}

function updateVisitCount() {
  let count = parseInt(localStorage.getItem('visitCount') || '0');
  count++;
  localStorage.setItem('visitCount', count.toString());

  const countElement = document.getElementById('visit-count');
  if (countElement) {
    countElement.textContent = count;
  }
}

function highlightCurrentPage() {
  const currentPath = window.location.pathname;
  const links = document.querySelectorAll('.navbar a');

  links.forEach(link => {
    if (link.getAttribute('href') === currentPath ||
        (currentPath === '/' && link.getAttribute('href') === '/')) {
      link.style.fontWeight = 'bold';
    }
  });
}

document.addEventListener('DOMContentLoaded', function() {
  updateTime();
  setInterval(updateTime, 1000);
  highlightCurrentPage();

  if (document.getElementById('visit-count')) {
    updateVisitCount();
  }
});
