let ws = null;
let state = {
  token: "",
  role: "",
  logged: false,
  rooms: [],
  selected_room_id: null,
  selected_room_name: "",
  joined_room_id: null,
  exam: { exam_id: -1, questions: [] },
  practice: { practice_id: -1, questions: [] },
  exam_end_time: 0,
  practice_end_time: 0,
  exam_auto_submitted: false,
  practice_auto_submitted: false,
  last_results_teacher: "",
  last_results_student: "",
};

const statusEl = document.getElementById("status");
const roleEl = document.getElementById("role");
const roomsTbody = document.querySelector("#rooms-table tbody");
const lobbyTbody = document.querySelector("#lobby-table tbody");
const examContainer = document.getElementById("exam-questions");
const pracContainer = document.getElementById("prac-questions");
const resultsTeachEl = document.getElementById("results-teacher");
const resultsStuEl = document.getElementById("results-student");
const errorsEl = document.getElementById("errors");
const navButtons = Array.from(document.querySelectorAll(".nav-btn"));
const noticeEl = document.getElementById("notice");
const examRoomInfo = document.getElementById("exam-room-info");
const toastContainer = document.getElementById("toast-container");
let roomsRefreshInterval = null;
let examTimerInterval = null;
let practiceTimerInterval = null;

function toast(msg, type = "info", timeout = 3000) {
  const div = document.createElement("div");
  div.className = `toast ${type}`;
  div.textContent = msg;
  toastContainer.appendChild(div);
  setTimeout(() => {
    div.remove();
  }, timeout);
}

function setStatus(text, ok = false) {
  statusEl.textContent = text;
  statusEl.style.background = ok ? "#10b981" : "#b91c1c";
}

function connectWs() {
  const host = document.getElementById("host").value || "127.0.0.1";
  const port = document.getElementById("port").value || "8080";
  ws = new WebSocket(`ws://${host}:${port}/ws`);
  ws.onopen = () => setStatus("Connected", true);
  ws.onclose = () => setStatus("Disconnected");
  ws.onerror = () => setStatus("Error");
  ws.onmessage = (ev) => handleMessage(JSON.parse(ev.data));
}

function send(action, data) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    setStatus("WS not connected");
    return;
  }
  ws.send(
    JSON.stringify({
      message_type: "REQUEST",
      action,
      timestamp: Math.floor(Date.now() / 1000),
      session_id: state.token,
      data,
    })
  );
}

function handleMessage(m) {
  if (m.status === "ERROR") {
    const msg = `${m.error_code}: ${m.error_message}`;
    errorsEl.textContent = msg;
    toast(msg, "error");
    return;
  }
  errorsEl.textContent = "";

  if (m.action === "REGISTER") {
    toast(`Registration successful! Welcome ${m.data.user_id}. You can now login.`, "success");
  } else if (m.action === "LOGIN") {
    state.token = m.session_id || m.data.session_id;
    state.role = m.data.role;
    state.logged = !!state.token;
    roleEl.textContent = state.role || "";
    noticeEl.textContent = state.logged
      ? `Logged in as ${state.role} (${state.token.slice(0, 8)}...)`
      : "Not logged in";
    enableNavForRole(state.role);
    if (state.role === "ADMIN") showPage("teacher-dashboard");
    else showPage("student-lobby");
    toast("Login successful", "success");
    startRoomsAutoRefresh();
  } else if (m.action === "LIST_ROOMS") {
    state.rooms = m.data.rooms || [];
    renderRooms();
    renderLobby();
  } else if (m.action === "JOIN_ROOM") {
    // join success
    state.joined_room_id = m.data.room_id;

    // Clear exam state cũ khi join room mới
    state.exam = { exam_id: -1, questions: [] };
    state.exam_auto_submitted = false;
    state.exam_end_time = 0;
    examContainer.innerHTML = "";

    // Stop old timer if exists
    if (examTimerInterval) clearInterval(examTimerInterval);
    if (state.exam_timer_sync_interval) clearInterval(state.exam_timer_sync_interval);
    examTimerInterval = null;
    state.exam_timer_sync_interval = null;

    // Clear timer display
    const timerEl = document.getElementById("exam-timer");
    if (timerEl) {
      timerEl.textContent = "Timer: --:--";
      timerEl.classList.remove("timer-warning", "timer-critical");
    }

    toast(`✅ Đã tham gia phòng #${m.data.room_id}`, "success");

    // Update buttons sau khi join
    updateExamButtonStates();

    // Auto refresh để cập nhật participant count từ server
    send("LIST_ROOMS", {});
  } else if (m.action === "GET_EXAM_PAPER") {
    state.exam.exam_id = m.data.exam_id;
    state.exam_auto_submitted = false;
    state.exam_end_time = m.data.end_time || 0;
    state.exam.questions = (m.data.questions || []).map((q) => ({
      id: q.question_id,
      text: q.question_text,
      difficulty: q.difficulty,
      topic: q.topic,
      options: q.options,
      answer: "",
    }));
    renderExam();
    showPage("student-exam");
    startExamTimer();
    updateExamButtonStates();  // Update button states after getting paper
    toast("Exam paper loaded", "info");
  } else if (m.action === "START_PRACTICE") {
    state.practice.practice_id = m.data.practice_id;
    state.practice_auto_submitted = false;
    state.practice_end_time = m.data.end_time || 0;
    state.practice.questions = (m.data.questions || []).map((q) => ({
      id: q.question_id,
      text: q.question_text,
      difficulty: q.difficulty,
      topic: q.topic,
      options: q.options,
      answer: "",
    }));
    renderPractice();
    showPage("student-practice");
    startPracticeTimer();
    toast("Practice started", "info");
  } else if (m.action === "GET_TIMER_STATUS") {
    // Update exam timer with server-authoritative time
    if (window.updateExamTimer && m.data.remaining_sec !== undefined) {
      window.updateExamTimer(m.data.remaining_sec);
    }
  } else if (m.action === "GET_ROOM_DETAILS") {
    // Display room details with improved UI
    const d = m.data;
    const participants = d.participants || [];

    let html = `<div style="padding: 20px; background: white; border-radius: 8px;">`;
    html += `<h2>${d.room_name}</h2>`;
    html += `<p style="color: #666;">${d.description || 'No description'}</p>`;
    html += `<div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 15px; margin: 20px 0;">`;
    html += `<div style="padding: 15px; background: #e3f2fd; border-radius: 5px;">
      <strong>Status</strong><br>${d.status}
    </div>`;
    html += `<div style="padding: 15px; background: #f3e5f5; border-radius: 5px;">
      <strong>Duration</strong><br>${d.duration_seconds} seconds
    </div>`;
    html += `<div style="padding: 15px; background: #e8f5e9; border-radius: 5px;">
      <strong>Participants</strong><br>${participants.length} students
    </div>`;
    html += `</div>`;

    html += `<h3>Participant List</h3>`;
    if (participants.length === 0) {
      html += '<p style="color: #666; padding: 20px;">No participants yet.</p>';
    } else {
      html += `<table style="width: 100%; border-collapse: collapse;">`;
      html += `<thead><tr style="background: #f0f0f0;">
        <th style="padding: 10px; text-align: left; border: 1px solid #ddd;">Name</th>
        <th style="padding: 10px; text-align: left; border: 1px solid #ddd;">Username</th>
        <th style="padding: 10px; text-align: center; border: 1px solid #ddd;">Status</th>
        <th style="padding: 10px; text-align: center; border: 1px solid #ddd;">Joined At</th>
      </tr></thead><tbody>`;

      participants.forEach(p => {
        const joinTime = new Date(p.joined_at * 1000).toLocaleString();
        html += `<tr>
          <td style="padding: 10px; border: 1px solid #ddd;">${p.full_name || 'N/A'}</td>
          <td style="padding: 10px; border: 1px solid #ddd;">${p.username}</td>
          <td style="padding: 10px; text-align: center; border: 1px solid #ddd;">
            <span class="pill">${p.status}</span>
          </td>
          <td style="padding: 10px; text-align: center; border: 1px solid #ddd;">
            ${joinTime}
          </td>
        </tr>`;
      });

      html += `</tbody></table>`;
    }
    html += `</div>`;

    resultsTeachEl.innerHTML = html;
    showPage("teacher-results");
    toast(`Room details loaded: ${participants.length} participant(s)`, "info");
  } else if (["SUBMIT_EXAM", "SUBMIT_PRACTICE", "GET_ROOM_RESULTS", "GET_USER_HISTORY"].includes(m.action)) {
    if (m.action === "GET_ROOM_RESULTS") {
      state.last_results_teacher = m.data; // Store object, not string
      displayCharts(m.data); // Use charts instead of raw JSON
      showPage("teacher-results");
    } else if (m.action === "GET_USER_HISTORY") {
      // Display history with beautiful formatting
      displayHistory(m.data);
      if (state.role === "STUDENT") showPage("student-history");
      else showPage("teacher-results");
    } else {
      // After submitting exam/practice, completely reset state
      if (m.action === "SUBMIT_EXAM") {
        // Stop all exam timers
        if (examTimerInterval) clearInterval(examTimerInterval);
        if (state.exam_timer_sync_interval) clearInterval(state.exam_timer_sync_interval);
        examTimerInterval = null;
        state.exam_timer_sync_interval = null;

        // Clear exam state completely
        state.exam = { exam_id: -1, questions: [] };
        state.exam_auto_submitted = true;
        state.exam_end_time = 0;

        // Clear exam UI - show success message
        examContainer.innerHTML = '<div style="text-align: center; padding: 60px;"><h2 style="color: #10b981;">✅ Đã nộp bài thành công!</h2><p style="color: #6b7280; margin-top: 20px;">Vui lòng chọn phòng thi khác hoặc xem lịch sử bài thi.</p></div>';

        // Reset timer display
        const timerEl = document.getElementById("exam-timer");
        if (timerEl) {
          timerEl.textContent = "Timer: --:--";
          timerEl.classList.remove("timer-warning", "timer-critical");
          timerEl.style.background = "";
        }

        // Reset buttons to initial state
        updateExamButtonStates();

      } else if (m.action === "SUBMIT_PRACTICE") {
        // Stop practice timer
        if (practiceTimerInterval) clearInterval(practiceTimerInterval);
        practiceTimerInterval = null;

        // Clear practice state
        state.practice = { practice_id: -1, questions: [] };
        state.practice_auto_submitted = true;
        state.practice_end_time = 0;
        pracContainer.innerHTML = '<div style="text-align: center; padding: 60px;"><h2 style="color: #10b981;">✅ Hoàn thành luyện tập!</h2></div>';
      }

      // Show history page with results
      displayHistory(m.data);
      showPage("student-history");
      send("GET_USER_HISTORY", {});
      toast("✅ Nộp bài thành công!", "success");
    }
  } else if (m.action === "DELETE_ROOM") {
    toast("Room deleted successfully", "success");
    send("LIST_ROOMS", {}); // Refresh room list
  } else if (m.action === "FINISH_ROOM") {
    toast("Room finished successfully", "success");
    send("LIST_ROOMS", {}); // Refresh room list
  } else if (m.action === "CREATE_ROOM") {
    if (m.data && m.data.room_id) {
      state.rooms.push({
        room_id: m.data.room_id,
        room_code: m.data.room_code || "",
        room_name: document.getElementById("room-name").value || "New room",
        status: m.data.status || "WAITING",
        duration_seconds: m.data.duration_seconds || 0,
        participant_count: 0,
      });
      renderRooms();
      renderLobby();
      toast(`Room created #${m.data.room_id}`, "success");
    } else {
      send("LIST_ROOMS", {});
    }
  } else if (m.action === "START_EXAM") {
    const rid = m.data.room_id;
    state.rooms = state.rooms.map((r) =>
      r.room_id === rid ? { ...r, status: m.data.status || "IN_PROGRESS" } : r
    );
    renderRooms();
    renderLobby();
    toast(`Room #${rid} started`, "info");
  }
}

function renderRooms() {
  roomsTbody.innerHTML = "";
  state.rooms.forEach((r) => {
    const tr = document.createElement("tr");

    // Create status badge with colors
    const statusColors = {
      WAITING: { bg: "#fbbf24", text: "#78350f" },      // Yellow
      IN_PROGRESS: { bg: "#10b981", text: "#065f46" },  // Green
      FINISHED: { bg: "#6b7280", text: "#ffffff" }      // Gray
    };
    const color = statusColors[r.status] || statusColors.FINISHED;
    const statusBadge = `<span class="pill" style="background: ${color.bg}; color: ${color.text}; font-weight: 600;">${r.status}</span>`;

    tr.innerHTML = `<td>${r.room_id}</td><td>${r.room_name}</td><td>${r.room_code}</td><td>${statusBadge}</td><td>${r.creator_name || 'N/A'}</td><td>${r.participant_count ?? 0}</td>`;
    const actions = document.createElement("td");

    // CHỈ hiển thị Start button cho Teacher
    if (state.role === "ADMIN") {
      const btnStart = document.createElement("button");
      btnStart.textContent = "Start";
      btnStart.disabled = (r.status !== "WAITING");
      btnStart.onclick = () => {
        toast("Đang bắt đầu phòng thi...", "info");
        send("START_EXAM", { room_id: r.room_id });
      };
      actions.appendChild(btnStart);

      // Add Finish button to manually end exam
      const btnFinish = document.createElement("button");
      btnFinish.textContent = "🏁 Finish";
      btnFinish.style.marginLeft = "5px";
      btnFinish.style.background = "#f97316";
      btnFinish.disabled = (r.status !== "IN_PROGRESS");
      btnFinish.onclick = () => {
        if (confirm(`Bạn có chắc muốn kết thúc phòng "${r.room_name}"? Tất cả học sinh chưa nộp bài sẽ được tự động nộp.`)) {
          toast("Đang kết thúc phòng thi...", "info");
          send("FINISH_ROOM", { room_id: r.room_id });
        }
      };
      actions.appendChild(btnFinish);

      // Add Results button to view room results
      const btnResults = document.createElement("button");
      btnResults.textContent = "📊 Results";
      btnResults.style.marginLeft = "5px";
      btnResults.style.background = "#10b981";
      btnResults.disabled = (r.status === "WAITING"); // Only show results for IN_PROGRESS or FINISHED
      btnResults.onclick = () => {
        // Switch to Results tab and load results for this room
        showPage("teacher-results");
        send("GET_ROOM_RESULTS", { room_id: r.room_id });
        toast(`Đang tải kết quả phòng ${r.room_name}...`, "info");
      };
      actions.appendChild(btnResults);

      // Add Details button to view participants
      const btnDetails = document.createElement("button");
      btnDetails.textContent = "👥 Details";
      btnDetails.style.marginLeft = "5px";
      btnDetails.onclick = () => {
        send("GET_ROOM_DETAILS", { room_id: r.room_id });
      };
      actions.appendChild(btnDetails);

      // Add Delete button (only for WAITING/FINISHED rooms)
      const btnDelete = document.createElement("button");
      btnDelete.textContent = "🗑️ Delete";
      btnDelete.style.marginLeft = "5px";
      btnDelete.style.background = "#f44336";
      btnDelete.disabled = (r.status === "IN_PROGRESS");
      btnDelete.onclick = () => {
        deleteRoom(r.room_id, r.room_name);
      };
      actions.appendChild(btnDelete);
    }

    tr.appendChild(actions);
    roomsTbody.appendChild(tr);
  });
}

function renderLobby() {
  lobbyTbody.innerHTML = "";

  // Separate rooms by status for better UX
  const waitingRooms = state.rooms.filter((r) => r.status === "WAITING");
  const inProgressRooms = state.rooms.filter((r) => r.status === "IN_PROGRESS");

  // Helper function to create status badge
  const createStatusBadge = (status) => {
    const colors = {
      WAITING: { bg: "#fbbf24", text: "#78350f" },      // Yellow/amber
      IN_PROGRESS: { bg: "#10b981", text: "#065f46" },  // Green
      FINISHED: { bg: "#6b7280", text: "#ffffff" }      // Gray
    };
    const color = colors[status] || colors.FINISHED;
    return `<span class="pill" style="background: ${color.bg}; color: ${color.text}; font-weight: 600;">${status}</span>`;
  };

  // Render WAITING rooms first (students can join these)
  if (waitingRooms.length > 0) {
    const headerRow = document.createElement("tr");
    headerRow.innerHTML = `<td colspan="5" style="background: #fffbeb; padding: 8px; font-weight: bold; color: #78350f; border-left: 4px solid #fbbf24;">🟡 Phòng đang chờ (${waitingRooms.length})</td>`;
    lobbyTbody.appendChild(headerRow);

    waitingRooms.forEach((r) => {
      const tr = document.createElement("tr");
      tr.style.background = "#fffbeb20";
      tr.innerHTML = `
        <td>${r.room_id}</td>
        <td><strong>${r.room_name}</strong></td>
        <td>${createStatusBadge(r.status)}</td>
        <td>${r.participant_count ?? 0} người</td>
      `;
      const actions = document.createElement("td");
      const btnSelect = document.createElement("button");
      btnSelect.textContent = "✅ Chọn & Tham gia";
      btnSelect.style.background = "#10b981";
      btnSelect.onclick = () => {
        state.selected_room_id = r.room_id;
        state.selected_room_name = r.room_name;
        updateExamRoomInfo();
        updateExamButtonStates();
        showPage("student-exam");
        toast(`Đã chọn phòng: ${r.room_name}`, "info");
      };
      actions.appendChild(btnSelect);
      tr.appendChild(actions);
      lobbyTbody.appendChild(tr);
    });
  }

  // Render IN_PROGRESS rooms (students can still join if they're participants)
  if (inProgressRooms.length > 0) {
    const headerRow = document.createElement("tr");
    headerRow.innerHTML = `<td colspan="5" style="background: #ecfdf5; padding: 8px; font-weight: bold; color: #065f46; border-left: 4px solid #10b981;">🟢 Phòng đang thi (${inProgressRooms.length})</td>`;
    lobbyTbody.appendChild(headerRow);

    inProgressRooms.forEach((r) => {
      const tr = document.createElement("tr");
      tr.style.background = "#ecfdf520";
      tr.innerHTML = `
        <td>${r.room_id}</td>
        <td><strong>${r.room_name}</strong></td>
        <td>${createStatusBadge(r.status)}</td>
        <td>${r.participant_count ?? 0} người</td>
      `;
      const actions = document.createElement("td");
      const btnSelect = document.createElement("button");
      btnSelect.textContent = "📝 Vào thi";
      btnSelect.style.background = "#f97316";
      btnSelect.onclick = () => {
        state.selected_room_id = r.room_id;
        state.selected_room_name = r.room_name;
        updateExamRoomInfo();
        updateExamButtonStates();
        showPage("student-exam");
        toast(`Đã chọn phòng: ${r.room_name}`, "info");
      };
      actions.appendChild(btnSelect);
      tr.appendChild(actions);
      lobbyTbody.appendChild(tr);
    });
  }

  // Show message if no rooms available
  if (waitingRooms.length === 0 && inProgressRooms.length === 0) {
    const emptyRow = document.createElement("tr");
    emptyRow.innerHTML = `<td colspan="5" style="text-align: center; padding: 30px; color: #6b7280;">Không có phòng thi nào. Vui lòng đợi giáo viên tạo phòng.</td>`;
    lobbyTbody.appendChild(emptyRow);
  }
}

function renderExam() {
  examContainer.innerHTML = "";

  // Add progress indicator
  if (state.exam.questions.length > 0) {
    const progressContainer = document.createElement("div");
    progressContainer.className = "question-progress";
    progressContainer.style.cssText = `
      position: sticky;
      top: 0;
      background: white;
      padding: 15px;
      border: 2px solid #8B0000;
      border-radius: 8px;
      margin-bottom: 20px;
      z-index: 100;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1);
    `;

    // Progress summary
    const answeredCount = state.exam.questions.filter(q => q.answer).length;
    const totalCount = state.exam.questions.length;
    const progressPercent = totalCount > 0 ? Math.round((answeredCount / totalCount) * 100) : 0;

    const summaryDiv = document.createElement("div");
    summaryDiv.style.cssText = "margin-bottom: 10px; font-weight: bold; color: #8B0000;";
    summaryDiv.textContent = `Progress: ${answeredCount}/${totalCount} (${progressPercent}%)`;
    progressContainer.appendChild(summaryDiv);

    // Question navigation grid
    const navGrid = document.createElement("div");
    navGrid.style.cssText = `
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(45px, 1fr));
      gap: 8px;
    `;

    state.exam.questions.forEach((q, index) => {
      const navBtn = document.createElement("button");
      navBtn.textContent = `${index + 1}`;
      navBtn.style.cssText = `
        padding: 10px;
        border: 2px solid ${q.answer ? '#10b981' : '#d1d5db'};
        background: ${q.answer ? '#10b981' : 'white'};
        color: ${q.answer ? 'white' : '#374151'};
        border-radius: 6px;
        cursor: pointer;
        font-weight: 600;
        transition: all 0.2s;
      `;
      navBtn.onmouseover = () => {
        if (!q.answer) {
          navBtn.style.background = '#f3f4f6';
        }
      };
      navBtn.onmouseout = () => {
        if (!q.answer) {
          navBtn.style.background = 'white';
        }
      };
      navBtn.onclick = () => {
        const questionCard = document.getElementById(`exam-q-${index}`);
        if (questionCard) {
          questionCard.scrollIntoView({ behavior: 'smooth', block: 'center' });
          questionCard.style.animation = 'highlight-question 1s';
        }
      };
      navGrid.appendChild(navBtn);
    });

    progressContainer.appendChild(navGrid);
    examContainer.appendChild(progressContainer);
  }

  // Render questions
  state.exam.questions.forEach((q, index) => {
    const card = document.createElement("div");
    card.className = "card";
    card.id = `exam-q-${index}`;
    card.innerHTML = `<div class="meta">Câu ${index + 1} · #${q.id} · ${q.difficulty} · ${q.topic}</div><div style="font-weight: 500; margin: 10px 0;">${q.text}</div>`;
    Object.entries(q.options).forEach(([key, val]) => {
      const lbl = document.createElement("label");
      lbl.style.flexDirection = "row";
      lbl.style.alignItems = "center";
      const input = document.createElement("input");
      input.type = "radio";
      input.name = `q-${q.id}`;
      input.value = key;
      input.checked = (q.answer === key);
      input.onchange = () => {
        q.answer = key;
        renderExam(); // Re-render to update progress
      };
      lbl.appendChild(input);
      const span = document.createElement("span");
      span.textContent = `${key}) ${val}`;
      span.style.marginLeft = "6px";
      lbl.appendChild(span);
      card.appendChild(lbl);
    });
    examContainer.appendChild(card);
  });
}

function renderPractice() {
  pracContainer.innerHTML = "";

  // Add progress indicator
  if (state.practice.questions.length > 0) {
    const progressContainer = document.createElement("div");
    progressContainer.className = "question-progress";
    progressContainer.style.cssText = `
      position: sticky;
      top: 0;
      background: white;
      padding: 15px;
      border: 2px solid #8B0000;
      border-radius: 8px;
      margin-bottom: 20px;
      z-index: 100;
      box-shadow: 0 2px 8px rgba(0,0,0,0.1);
    `;

    // Progress summary
    const answeredCount = state.practice.questions.filter(q => q.answer).length;
    const totalCount = state.practice.questions.length;
    const progressPercent = totalCount > 0 ? Math.round((answeredCount / totalCount) * 100) : 0;

    const summaryDiv = document.createElement("div");
    summaryDiv.style.cssText = "margin-bottom: 10px; font-weight: bold; color: #8B0000;";
    summaryDiv.textContent = `Progress: ${answeredCount}/${totalCount} (${progressPercent}%)`;
    progressContainer.appendChild(summaryDiv);

    // Question navigation grid
    const navGrid = document.createElement("div");
    navGrid.style.cssText = `
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(45px, 1fr));
      gap: 8px;
    `;

    state.practice.questions.forEach((q, index) => {
      const navBtn = document.createElement("button");
      navBtn.textContent = `${index + 1}`;
      navBtn.style.cssText = `
        padding: 10px;
        border: 2px solid ${q.answer ? '#10b981' : '#d1d5db'};
        background: ${q.answer ? '#10b981' : 'white'};
        color: ${q.answer ? 'white' : '#374151'};
        border-radius: 6px;
        cursor: pointer;
        font-weight: 600;
        transition: all 0.2s;
      `;
      navBtn.onmouseover = () => {
        if (!q.answer) {
          navBtn.style.background = '#f3f4f6';
        }
      };
      navBtn.onmouseout = () => {
        if (!q.answer) {
          navBtn.style.background = 'white';
        }
      };
      navBtn.onclick = () => {
        const questionCard = document.getElementById(`prac-q-${index}`);
        if (questionCard) {
          questionCard.scrollIntoView({ behavior: 'smooth', block: 'center' });
          questionCard.style.animation = 'highlight-question 1s';
        }
      };
      navGrid.appendChild(navBtn);
    });

    progressContainer.appendChild(navGrid);
    pracContainer.appendChild(progressContainer);
  }

  // Render questions
  state.practice.questions.forEach((q, index) => {
    const card = document.createElement("div");
    card.className = "card";
    card.id = `prac-q-${index}`;
    card.innerHTML = `<div class="meta">Câu ${index + 1} · #${q.id} · ${q.difficulty} · ${q.topic}</div><div style="font-weight: 500; margin: 10px 0;">${q.text}</div>`;
    Object.entries(q.options).forEach(([key, val]) => {
      const lbl = document.createElement("label");
      lbl.style.flexDirection = "row";
      lbl.style.alignItems = "center";
      const input = document.createElement("input");
      input.type = "radio";
      input.name = `p-${q.id}`;
      input.value = key;
      input.checked = (q.answer === key);
      input.onchange = () => {
        q.answer = key;
        renderPractice(); // Re-render to update progress
      };
      lbl.appendChild(input);
      const span = document.createElement("span");
      span.textContent = `${key}) ${val}`;
      span.style.marginLeft = "6px";
      lbl.appendChild(span);
      card.appendChild(lbl);
    });
    pracContainer.appendChild(card);
  });
}

// Wire buttons
document.getElementById("btn-connect").onclick = connectWs;
document.getElementById("btn-login").onclick = () => {
  const u = document.getElementById("username").value;
  const p = document.getElementById("password").value;
  send("LOGIN", { username: u, password: p });
};

// Register Modal Logic
const registerModal = document.getElementById("register-modal");
const btnRegister = document.getElementById("btn-register");
const btnRegisterSubmit = document.getElementById("btn-register-submit");
const btnRegisterCancel = document.getElementById("btn-register-cancel");
const closeRegisterModal = document.getElementById("close-register-modal");

btnRegister.onclick = () => {
  registerModal.classList.add("show");
  // Clear form
  document.getElementById("reg-username").value = "";
  document.getElementById("reg-password").value = "";
  document.getElementById("reg-fullname").value = "";
  document.getElementById("reg-email").value = "";
};

closeRegisterModal.onclick = () => {
  registerModal.classList.remove("show");
};

btnRegisterCancel.onclick = () => {
  registerModal.classList.remove("show");
};

btnRegisterSubmit.onclick = () => {
  const username = document.getElementById("reg-username").value.trim();
  const password = document.getElementById("reg-password").value.trim();
  const fullname = document.getElementById("reg-fullname").value.trim();
  const email = document.getElementById("reg-email").value.trim();

  if (!username || !password || !fullname) {
    toast("Username, Password, and Full Name are required", "error");
    return;
  }

  send("REGISTER", {
    username: username,
    password: password,
    full_name: fullname,
    email: email
  });

  registerModal.classList.remove("show");
};

// Close modal when clicking outside
registerModal.onclick = (e) => {
  if (e.target === registerModal) {
    registerModal.classList.remove("show");
  }
};

document.getElementById("btn-refresh").onclick = () => send("LIST_ROOMS", {});
document.getElementById("btn-create").onclick = () => {
  send("CREATE_ROOM", {
    room_name: document.getElementById("room-name").value,
    description: document.getElementById("room-desc").value,
    duration_minutes: parseInt(document.getElementById("room-dur").value || "30", 10),
    room_pass: document.getElementById("room-pass").value || "",
    question_settings: {
      total_questions: parseInt(document.getElementById("room-total").value || "10", 10),
      difficulty_distribution: {
        easy: parseInt(document.getElementById("room-easy").value || "4", 10),
        medium: parseInt(document.getElementById("room-med").value || "4", 10),
        hard: parseInt(document.getElementById("room-hard").value || "2", 10),
      },
    },
  });
};
// Refresh lobby (all rooms; filter applied in renderLobby)
document.getElementById("btn-refresh-lobby").onclick = () => send("LIST_ROOMS", {});
document.getElementById("btn-join-room").onclick = () => {
  if (!state.selected_room_id) {
    toast("Hãy chọn phòng thi từ lobby trước", "error");
    return;
  }
  const pass = document.getElementById("exam-pass").value || "";
  if (!pass) {
    toast("Vui lòng nhập mật khẩu phòng", "error");
    return;
  }
  toast("Đang tham gia phòng...", "info");
  send("JOIN_ROOM", { room_id: state.selected_room_id, room_pass: pass });
};
document.getElementById("btn-get-paper").onclick = () => {
  if (!state.selected_room_id) {
    toast("Hãy chọn phòng thi từ lobby trước", "error");
    return;
  }
  if (state.joined_room_id !== state.selected_room_id) {
    toast("⚠️ Bạn phải tham gia phòng trước!", "warning");
    return;
  }
  toast("Đang tải đề thi...", "info");
  send("GET_EXAM_PAPER", { room_id: state.selected_room_id });
};
document.getElementById("btn-submit-exam").onclick = () => {
  if (state.joined_room_id !== state.selected_room_id) {
    toast("⚠️ Bạn phải tham gia phòng trước!", "warning");
    return;
  }
  // Confirmation dialog
  if (!confirm("Bạn có chắc muốn nộp bài? Bạn sẽ không thể sửa đáp án sau khi nộp.")) {
    return;
  }
  const answers = state.exam.questions.map((q) => ({
    question_id: q.id,
    selected_option: q.answer || "A",
  }));
  toast("Đang nộp bài...", "info");
  send("SUBMIT_EXAM", { exam_id: state.exam.exam_id, final_answers: answers });
};
document.getElementById("btn-start-prac").onclick = () => {
  send("START_PRACTICE", {
    question_count: parseInt(document.getElementById("prac-count").value || "6", 10),
    duration_minutes: parseInt(document.getElementById("prac-dur").value || "10", 10),
    difficulty_filter: [],  // Empty = all difficulties
    topic_filter: [],       // Empty = all topics
  });
};
document.getElementById("btn-submit-prac").onclick = () => {
  const answers = state.practice.questions.map((q) => ({
    question_id: q.id,
    selected_option: q.answer || "A",
  }));
  send("SUBMIT_PRACTICE", { practice_id: state.practice.practice_id, final_answers: answers });
};
document.getElementById("btn-room-results").onclick = () => {
  const rid = parseInt(document.getElementById("res-room").value || "-1", 10);
  send("GET_ROOM_RESULTS", { room_id: rid });
};
document.getElementById("btn-history-teacher").onclick = () => send("GET_USER_HISTORY", {});
document.getElementById("btn-history-student").onclick = () => send("GET_USER_HISTORY", {});

function showPage(pageId) {
  document.querySelectorAll(".page").forEach((p) => p.classList.remove("active"));
  const el = document.getElementById(pageId);
  if (el) el.classList.add("active");
  navButtons.forEach((b) => b.classList.toggle("active", b.dataset.page === pageId));
}
navButtons.forEach((btn) => {
  btn.onclick = () => showPage(btn.dataset.page);
});

function enableNavForRole(role) {
  navButtons.forEach((btn) => {
    const allowed = btn.dataset.role === role;
    btn.classList.toggle("disabled", !allowed);
    if (!allowed) btn.classList.remove("active");
  });
}

function updateExamRoomInfo() {
  if (state.selected_room_id) {
    examRoomInfo.textContent = `Room: ${state.selected_room_name} (#${state.selected_room_id})`;
  } else {
    examRoomInfo.textContent = "Room: (select from lobby)";
  }
}

function updateExamButtonStates() {
  const joined = (state.joined_room_id === state.selected_room_id);
  const hasExam = (state.exam.exam_id > 0);
  const btnJoin = document.getElementById("btn-join-room");
  const btnGetPaper = document.getElementById("btn-get-paper");
  const btnSubmit = document.getElementById("btn-submit-exam");

  // Join button: disable nếu chưa select room HOẶC đã join rồi
  if (btnJoin) {
    btnJoin.disabled = !state.selected_room_id || joined;
    if (joined) {
      btnJoin.textContent = "✅ Đã tham gia";
      btnJoin.style.background = "#6b7280";  // Gray
    } else {
      btnJoin.textContent = "Tham gia phòng";
      btnJoin.style.background = "";
    }
  }

  // Get Paper button: disable nếu chưa join HOẶC đã get paper rồi
  if (btnGetPaper) {
    btnGetPaper.disabled = !joined || hasExam;
    if (hasExam) {
      btnGetPaper.textContent = "✅ Đã lấy đề";
      btnGetPaper.style.background = "#6b7280";  // Gray
    } else {
      btnGetPaper.textContent = "Lấy đề thi";
      btnGetPaper.style.background = "";
    }
  }

  // Submit button: chỉ enable khi đã get paper
  if (btnSubmit) btnSubmit.disabled = !hasExam;
}

function startRoomsAutoRefresh() {
  if (roomsRefreshInterval) clearInterval(roomsRefreshInterval);
  roomsRefreshInterval = setInterval(() => send("LIST_ROOMS", {}), 5000);
}

function formatRemaining(endSec) {
  const now = Math.floor(Date.now() / 1000);
  const rem = Math.max(0, endSec - now);
  const mm = String(Math.floor(rem / 60)).padStart(2, "0");
  const ss = String(rem % 60).padStart(2, "0");
  return { text: `${mm}:${ss}`, rem };
}

function startExamTimer() {
  clearInterval(examTimerInterval);
  const el = document.getElementById("exam-timer");
  if (!state.exam.exam_id || state.exam.exam_id <= 0 || !el) return;

  let lastWarning = null;

  // Sync with server every 5 seconds for authoritative time
  const syncWithServer = () => {
    send("GET_TIMER_STATUS", { exam_id: state.exam.exam_id });
  };

  // Initial sync
  syncWithServer();

  // Local countdown between syncs (updates every second)
  let localRemaining = null;
  examTimerInterval = setInterval(() => {
    if (localRemaining !== null && localRemaining > 0) {
      localRemaining--;
      const mm = Math.floor(localRemaining / 60).toString().padStart(2, "0");
      const ss = (localRemaining % 60).toString().padStart(2, "0");
      el.textContent = `Timer: ${mm}:${ss}`;

      // Timer warnings with visual feedback
      if (localRemaining === 300 && lastWarning !== 300) {
        lastWarning = 300;
        el.classList.add("timer-warning");
        toast("⏰ 5 minutes remaining!", "warning", 3000);
      } else if (localRemaining === 60 && lastWarning !== 60) {
        lastWarning = 60;
        el.classList.add("timer-critical");
        toast("⚠️ 1 minute remaining!", "error", 3000);
      } else if (localRemaining === 30 && lastWarning !== 30) {
        lastWarning = 30;
        toast("🚨 30 seconds remaining!", "error", 3000);
      } else if (localRemaining > 300) {
        el.classList.remove("timer-warning", "timer-critical");
      } else if (localRemaining > 60) {
        el.classList.remove("timer-critical");
      }

      if (localRemaining === 0) {
        toast("⏰ Hết giờ - Tự động nộp bài", "error", 4000);
        clearInterval(examTimerInterval);
        clearInterval(serverSyncInterval);
        autoSubmitExam();
      }
    }
  }, 1000);

  // Server sync every 5 seconds
  const serverSyncInterval = setInterval(() => {
    // Double check exam_id is still valid before syncing
    if (state.exam && state.exam.exam_id && state.exam.exam_id > 0 && !state.exam_auto_submitted) {
      syncWithServer();
    } else {
      clearInterval(serverSyncInterval);
      clearInterval(examTimerInterval);
    }
  }, 5000);

  // Store interval reference for cleanup
  state.exam_timer_sync_interval = serverSyncInterval;

  // Update local countdown when server responds
  window.updateExamTimer = (remaining_sec) => {
    localRemaining = remaining_sec > 0 ? remaining_sec : 0;
    if (localRemaining === 0 && !state.exam_auto_submitted) {
      toast("⏰ Hết giờ - Tự động nộp bài", "error", 4000);
      clearInterval(examTimerInterval);
      clearInterval(serverSyncInterval);
      autoSubmitExam();
    }
  };
}

function startPracticeTimer() {
  clearInterval(practiceTimerInterval);
  const el = document.getElementById("prac-timer");
  if (!state.practice_end_time || !el) return;
  practiceTimerInterval = setInterval(() => {
    const { text, rem } = formatRemaining(state.practice_end_time);
    el.textContent = `Timer: ${text}`;
    if (rem === 0) {
      toast("Practice time over - auto submitting", "error", 4000);
      autoSubmitPractice();
      clearInterval(practiceTimerInterval);
    }
  }, 1000);
}

function autoSubmitExam() {
  if (state.exam_auto_submitted) return;
  if (state.joined_room_id !== state.selected_room_id || state.exam.exam_id <= 0) return;

  state.exam_auto_submitted = true;

  // Stop all timers immediately
  if (examTimerInterval) clearInterval(examTimerInterval);
  if (state.exam_timer_sync_interval) clearInterval(state.exam_timer_sync_interval);
  examTimerInterval = null;
  state.exam_timer_sync_interval = null;

  const answers = state.exam.questions.map((q) => ({
    question_id: q.id,
    selected_option: q.answer || "A",
  }));
  send("SUBMIT_EXAM", { exam_id: state.exam.exam_id, final_answers: answers });
}

function autoSubmitPractice() {
  if (state.practice_auto_submitted) return;
  if (state.practice.practice_id <= 0) return;
  state.practice_auto_submitted = true;
  const answers = state.practice.questions.map((q) => ({
    question_id: q.id,
    selected_option: q.answer || "A",
  }));
  send("SUBMIT_PRACTICE", { practice_id: state.practice.practice_id, final_answers: answers });
}

// ========== CHART RENDERING FUNCTIONS ==========

let currentCharts = {
  scoreDistribution: null,
  passFail: null
};

// Destroy existing charts to prevent memory leaks
function destroyCharts() {
  Object.values(currentCharts).forEach(chart => {
    if (chart) chart.destroy();
  });
  currentCharts = {
    scoreDistribution: null,
    passFail: null
  };
}

// Render Score Distribution Chart (Bar/Histogram)
function renderScoreDistribution(results) {
  const ctx = document.getElementById('chart-score-distribution').getContext('2d');

  // Group scores into bins: 0-20, 21-40, 41-60, 61-80, 81-100
  const bins = {
    '0-20': 0,
    '21-40': 0,
    '41-60': 0,
    '61-80': 0,
    '81-100': 0
  };

  results.participants.forEach(p => {
    const scorePercent = (p.score || 0) * 10; // Convert from 0-10 scale to 0-100%
    if (scorePercent <= 20) bins['0-20']++;
    else if (scorePercent <= 40) bins['21-40']++;
    else if (scorePercent <= 60) bins['41-60']++;
    else if (scorePercent <= 80) bins['61-80']++;
    else bins['81-100']++;
  });

  currentCharts.scoreDistribution = new Chart(ctx, {
    type: 'bar',
    data: {
      labels: Object.keys(bins),
      datasets: [{
        label: 'Number of Students',
        data: Object.values(bins),
        backgroundColor: [
          'rgba(255, 99, 132, 0.7)',   // Red for 0-20
          'rgba(255, 159, 64, 0.7)',   // Orange for 21-40
          'rgba(255, 205, 86, 0.7)',   // Yellow for 41-60
          'rgba(75, 192, 192, 0.7)',   // Teal for 61-80
          'rgba(54, 162, 235, 0.7)'    // Blue for 81-100
        ],
        borderColor: [
          'rgba(255, 99, 132, 1)',
          'rgba(255, 159, 64, 1)',
          'rgba(255, 205, 86, 1)',
          'rgba(75, 192, 192, 1)',
          'rgba(54, 162, 235, 1)'
        ],
        borderWidth: 2
      }]
    },
    options: {
      responsive: true,
      plugins: {
        legend: {
          display: false
        },
        title: {
          display: true,
          text: 'Score Distribution (%)'
        }
      },
      scales: {
        y: {
          beginAtZero: true,
          ticks: {
            stepSize: 1
          },
          title: {
            display: true,
            text: 'Number of Students'
          }
        },
        x: {
          title: {
            display: true,
            text: 'Score Range (%)'
          }
        }
      }
    }
  });
}

// Render Pass/Fail Rate Chart (Doughnut)
function renderPassFailChart(results) {
  const ctx = document.getElementById('chart-pass-fail').getContext('2d');

  const totalStudents = results.participants.length;
  const passThreshold = 5.0; // 5.0/10 = 50% to pass
  let passed = 0;
  let failed = 0;

  results.participants.forEach(p => {
    const score = p.score || 0;
    if (score >= passThreshold) passed++;
    else failed++;
  });

  currentCharts.passFail = new Chart(ctx, {
    type: 'doughnut',
    data: {
      labels: ['Passed', 'Failed'],
      datasets: [{
        data: [passed, failed],
        backgroundColor: [
          'rgba(75, 192, 192, 0.8)',   // Green for passed
          'rgba(255, 99, 132, 0.8)'    // Red for failed
        ],
        borderColor: [
          'rgba(75, 192, 192, 1)',
          'rgba(255, 99, 132, 1)'
        ],
        borderWidth: 2
      }]
    },
    options: {
      responsive: true,
      plugins: {
        legend: {
          position: 'bottom'
        },
        title: {
          display: true,
          text: `Pass Rate: ${totalStudents > 0 ? ((passed/totalStudents)*100).toFixed(1) : 0}%`
        },
        tooltip: {
          callbacks: {
            label: function(context) {
              const label = context.label || '';
              const value = context.parsed || 0;
              const percentage = totalStudents > 0 ? ((value/totalStudents)*100).toFixed(1) : 0;
              return `${label}: ${value} students (${percentage}%)`;
            }
          }
        }
      }
    }
  });
}

// Render Results Table
function renderResultsTable(results) {
  const container = document.getElementById('results-table-container');

  let html = '<h3>Detailed Results</h3>';
  html += '<table style="width: 100%; border-collapse: collapse; margin-top: 10px;">';
  html += '<thead><tr style="background: #f0f0f0;">';
  html += '<th style="padding: 8px; border: 1px solid #ddd;">Rank</th>';
  html += '<th style="padding: 8px; border: 1px solid #ddd;">Student</th>';
  html += '<th style="padding: 8px; border: 1px solid #ddd;">Score (%)</th>';
  html += '<th style="padding: 8px; border: 1px solid #ddd;">Correct/Total</th>';
  html += '<th style="padding: 8px; border: 1px solid #ddd;">Status</th>';
  html += '</tr></thead><tbody>';

  // Sort by score descending
  const sorted = [...results.participants].sort((a, b) => (b.score || 0) - (a.score || 0));

  sorted.forEach((p, idx) => {
    const score = p.score || 0;
    const scorePercent = score * 10; // Convert from 0-10 scale to 0-100%
    const status = score >= 5.0 ? '✅ Passed' : '❌ Failed'; // Pass threshold is 5.0/10
    const rowColor = score >= 5.0 ? '#e8f5e9' : '#ffebee';

    html += `<tr style="background: ${rowColor};">`;
    html += `<td style="padding: 8px; border: 1px solid #ddd; text-align: center;">${idx + 1}</td>`;
    html += `<td style="padding: 8px; border: 1px solid #ddd;">${p.username || p.full_name || 'Unknown'}</td>`;
    html += `<td style="padding: 8px; border: 1px solid #ddd; text-align: center; font-weight: bold;">${scorePercent.toFixed(1)}%</td>`;
    html += `<td style="padding: 8px; border: 1px solid #ddd; text-align: center;">${p.correct || 0}/${p.total || 0}</td>`;
    html += `<td style="padding: 8px; border: 1px solid #ddd; text-align: center;">${status}</td>`;
    html += '</tr>';
  });

  html += '</tbody></table>';

  // Statistics summary
  html += '<div style="margin-top: 20px; padding: 15px; background: #f5f5f5; border-radius: 5px;">';
  html += '<h4>Statistics Summary</h4>';
  html += `<p><strong>Average Score:</strong> ${((results.statistics?.average_score || 0) * 10).toFixed(1)}%</p>`;
  html += `<p><strong>Highest Score:</strong> ${((results.statistics?.highest_score || 0) * 10).toFixed(1)}%</p>`;
  html += `<p><strong>Lowest Score:</strong> ${((results.statistics?.lowest_score || 0) * 10).toFixed(1)}%</p>`;
  html += `<p><strong>Pass Rate:</strong> ${(results.statistics?.pass_rate || 0).toFixed(1)}%</p>`;
  html += '</div>';

  container.innerHTML = html;
}

// Main function to display charts
function displayCharts(resultsData) {
  // Destroy old charts
  destroyCharts();

  // Show charts container, hide raw JSON
  document.getElementById('charts-container').style.display = 'block';
  document.getElementById('results-teacher').style.display = 'none';

  // Render all charts
  if (resultsData.participants && resultsData.participants.length > 0) {
    renderScoreDistribution(resultsData);
    renderPassFailChart(resultsData);
    renderResultsTable(resultsData);
  } else {
    // No data
    document.getElementById('results-table-container').innerHTML =
      '<p style="color: #666; padding: 20px;">No results available for this room.</p>';
  }
}

// ========== DISPLAY HISTORY FUNCTION ==========

function displayHistory(historyData) {
  const container = document.getElementById('results-student');

  if (!historyData || (!historyData.exams && !historyData.practices)) {
    container.innerHTML = '<p style="color: #64748b; padding: 20px;">No history available.</p>';
    return;
  }

  let html = '';

  // Average Score Summary
  html += '<div style="margin-bottom: 24px; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); border-radius: 12px; color: white;">';
  html += '<h3 style="margin: 0 0 8px 0; font-size: 24px;">Your Average Score</h3>';
  html += `<p style="margin: 0; font-size: 36px; font-weight: 700;">${(historyData.avg_score || 0).toFixed(1)}%</p>`;
  html += '</div>';

  // Exam History Table
  if (historyData.exams && historyData.exams.length > 0) {
    html += '<div style="margin-bottom: 24px;">';
    html += '<h3 style="margin: 0 0 12px 0; color: #1e293b; font-size: 20px;">📝 Exam History</h3>';
    html += '<table style="width: 100%; border-collapse: collapse; background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 3px rgba(0,0,0,0.1);">';
    html += '<thead><tr style="background: #f8fafc;">';
    html += '<th style="padding: 12px; text-align: left; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Room Name</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Score</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Correct/Total</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Submitted At</th>';
    html += '</tr></thead><tbody>';

    historyData.exams.forEach((exam, idx) => {
      const score = exam.score || 0;
      const scoreColor = score >= 80 ? '#10b981' : score >= 50 ? '#f59e0b' : '#ef4444';
      const bgColor = idx % 2 === 0 ? '#ffffff' : '#f8fafc';
      const date = new Date((exam.submitted_at || 0) * 1000).toLocaleString();

      html += `<tr style="background: ${bgColor};">`;
      html += `<td style="padding: 12px; color: #1e293b; border-bottom: 1px solid #e2e8f0;">${exam.room_name || 'N/A'}</td>`;
      html += `<td style="padding: 12px; text-align: center; font-weight: 700; font-size: 16px; color: ${scoreColor}; border-bottom: 1px solid #e2e8f0;">${score.toFixed(1)}%</td>`;
      html += `<td style="padding: 12px; text-align: center; color: #475569; border-bottom: 1px solid #e2e8f0;">${exam.correct || 0}/${exam.total || 0}</td>`;
      html += `<td style="padding: 12px; text-align: center; color: #64748b; font-size: 13px; border-bottom: 1px solid #e2e8f0;">${date}</td>`;
      html += '</tr>';
    });

    html += '</tbody></table></div>';
  }

  // Practice History Table
  if (historyData.practices && historyData.practices.length > 0) {
    html += '<div>';
    html += '<h3 style="margin: 0 0 12px 0; color: #1e293b; font-size: 20px;">💪 Practice History</h3>';
    html += '<table style="width: 100%; border-collapse: collapse; background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 3px rgba(0,0,0,0.1);">';
    html += '<thead><tr style="background: #f8fafc;">';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Practice #</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Score</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Correct/Total</th>';
    html += '<th style="padding: 12px; text-align: center; color: #6366f1; font-weight: 600; border-bottom: 2px solid #e2e8f0;">Submitted At</th>';
    html += '</tr></thead><tbody>';

    historyData.practices.forEach((practice, idx) => {
      const score = practice.score || 0;
      const scoreColor = score >= 80 ? '#10b981' : score >= 50 ? '#f59e0b' : '#ef4444';
      const bgColor = idx % 2 === 0 ? '#ffffff' : '#f8fafc';
      const date = new Date((practice.submitted_at || 0) * 1000).toLocaleString();

      html += `<tr style="background: ${bgColor};">`;
      html += `<td style="padding: 12px; text-align: center; color: #1e293b; font-weight: 600; border-bottom: 1px solid #e2e8f0;">#${idx + 1}</td>`;
      html += `<td style="padding: 12px; text-align: center; font-weight: 700; font-size: 16px; color: ${scoreColor}; border-bottom: 1px solid #e2e8f0;">${score.toFixed(1)}%</td>`;
      html += `<td style="padding: 12px; text-align: center; color: #475569; border-bottom: 1px solid #e2e8f0;">${practice.correct || 0}/${practice.total || 0}</td>`;
      html += `<td style="padding: 12px; text-align: center; color: #64748b; font-size: 13px; border-bottom: 1px solid #e2e8f0;">${date}</td>`;
      html += '</tr>';
    });

    html += '</tbody></table></div>';
  }

  // No history message
  if ((!historyData.exams || historyData.exams.length === 0) &&
      (!historyData.practices || historyData.practices.length === 0)) {
    html += '<p style="color: #64748b; padding: 20px; text-align: center;">No exam or practice records found.</p>';
  }

  container.innerHTML = html;
}

// ========== DELETE ROOM FUNCTION ==========

// Delete room with confirmation
function deleteRoom(roomId, roomName) {
  if (!confirm(`Are you sure you want to delete room "${roomName}"?\n\nThis action cannot be undone.`)) {
    return;
  }

  send("DELETE_ROOM", { room_id: roomId });
}

showPage("teacher-dashboard");
