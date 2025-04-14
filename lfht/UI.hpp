#pragma once
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include "TestSettings.hpp"

class UI
{
public:

	UI(TestSettings* setting) : 
		m_pWindow(nullptr),
		m_pTestSettings(setting),
		m_keyInput(0),
		m_numThreadsSlider(4),
		m_bucketCountSlider(16),
		m_limitOps(true)
	{
	}

	~UI()
	{
		delete m_pTestSettings;
		m_pTestSettings = nullptr;
	}

	bool Init()
	{
        if (!glfwInit())
        {
            return false;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

        m_pWindow = glfwCreateWindow(1280, 720, "LockFreeHashTable Visualization", nullptr, nullptr);
        if (!m_pWindow)
        {
            return false;
        }
        glfwMakeContextCurrent(m_pWindow);
        glfwSwapInterval(1);

        if (glewInit() != GLEW_OK)
        {
			return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(m_pWindow, true);
        ImGui_ImplOpenGL3_Init("#version 330");

		return true;
	}

	void Update()
	{
		while (!glfwWindowShouldClose(m_pWindow))
		{
			glfwPollEvents();
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();

			simulationControls();
			bucketListing();
			loadFactorGraph();
			bucketHistogram();
			operations();
			opsPerThread();

			ImGui::Render();
			int display_w, display_h;
			glfwGetFramebufferSize(m_pWindow, &display_w, &display_h);
			glViewport(0, 0, display_w, display_h);
			glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			glfwSwapBuffers(m_pWindow);
		}
	}

	void Cleanup()
	{
		m_pTestSettings->Reset();

		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(m_pWindow);
		glfwTerminate();
	}


private:
    GLFWwindow* m_pWindow;
	TestSettings* m_pTestSettings;

	int m_keyInput;
	int m_numThreadsSlider;
	int m_bucketCountSlider;
	char m_valueInput[32] = "value";
	bool m_limitOps;

	void simulationControls()
	{
		ImGui::Begin("Simulation Controls");
		ImGui::InputInt("Key", &m_keyInput);
		ImGui::InputText("Value", m_valueInput, IM_ARRAYSIZE(m_valueInput));
		if (ImGui::Button("Insert"))
		{
			if (m_pTestSettings->GetVisualTable()->Insert(m_keyInput, m_valueInput))
				m_pTestSettings->AddInsertOpCount(1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove"))
		{
			if (m_pTestSettings->GetVisualTable()->Remove(m_keyInput))
				m_pTestSettings->AddRemoveOpCount(1);
		}
		ImGui::Separator();

		if (ImGui::SliderInt("Number of Buckets", reinterpret_cast<int*>(&m_bucketCountSlider), 16, 256))
		{
			m_pTestSettings->GetVisualTable()->AdjustBucketCount(static_cast<size_t>(m_bucketCountSlider));
		}

		ImGui::Separator();
		ImGui::SliderInt("Worker Threads", &m_numThreadsSlider, 1, 16);
		if (ImGui::Button("Start Workers"))
		{
			m_pTestSettings->SetRunWorkers(true);
			for (int i = 0; i < m_numThreadsSlider; ++i)
			{
				m_pTestSettings->GetWorkers().emplace_back(
					&TestSettings::WorkerFunction,   
					m_pTestSettings,                 
					i                                
				);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Stop Workers"))
		{
			m_pTestSettings->SetRunWorkers(false);
			for (auto& t : m_pTestSettings->GetWorkers())
			{
				if (t.joinable())
					t.join();
			}
			m_pTestSettings->GetWorkers().clear();
		}

		ImGui::SameLine();

		if (ImGui::Checkbox("Limit Ops Speed", &m_limitOps))
		{
			m_pTestSettings->SetLimitOps(!m_limitOps);
		}
		ImGui::End();
	}
	 
	void bucketListing()
	{
		ImGui::Begin("Bucket Listing");
		auto snapshot = m_pTestSettings->GetVisualTable()->GetSnapshot();

		for (size_t i = 0; i < snapshot.size(); ++i)
		{
			std::stringstream header;
			int activeCount = 0;
			for (const auto& node : snapshot[i])
			{
				if (!std::get<2>(node))
					activeCount++;
			}
			header << "Bucket " << i << " (" << activeCount << " nodes)";
			if (ImGui::CollapsingHeader(header.str().c_str()))
			{
				for (const auto& node : snapshot[i])
				{
					int key;
					std::string val;
					bool marked;
					std::tie(key, val, marked) = node;
					std::stringstream nodeStr;
					nodeStr << "Key: " << key << ", Val: " << val << (marked ? " [Marked]" : "");
					ImGui::Text("%s", nodeStr.str().c_str());
				}
			}
		}

		ImGui::End();
	}
	void loadFactorGraph()
	{
		ImGui::Begin("Load Factor Graph");
		m_pTestSettings->UpdateLoadFactorHistory();
		auto loadFactorHistory = m_pTestSettings->GetLoadFactorHistory();
		if (!loadFactorHistory.empty())
		{
			ImGui::PlotLines("Load Factor", loadFactorHistory.data(),
				static_cast<int>(loadFactorHistory.size()),
				0, "Active Load Factor", 0.0f, 5.0f, ImVec2(600, 200));
		}
		ImGui::End();
	}
	void bucketHistogram()
	{
		ImGui::Begin("Bucket Histogram");
		auto bucketInfos = m_pTestSettings->GetBucketInfoSnapshot();
		std::vector<float> bucketCounts;
		for (const auto& b : bucketInfos)
		{
			bucketCounts.push_back(static_cast<float>(b.nodeCount));
		}
		float maxBucket = 1.0f;
		if (!bucketCounts.empty())
		{
			maxBucket = *std::max_element(bucketCounts.begin(), bucketCounts.end());
			if (maxBucket < 1.0f)
				maxBucket = 1.0f;
		}
		ImGui::PlotHistogram("Bucket Sizes", bucketCounts.data(), static_cast<int>(bucketCounts.size()),
			0, "Active Nodes per Bucket", 0.0f, maxBucket + 1.0f, ImVec2(600, 200));
		ImGui::Text("Total Buckets: %d", (int)bucketCounts.size());
		ImGui::End();
	}
	void operations()
	{
		ImGui::Begin("Operations");
		int insertOps = m_pTestSettings->GetOpInsertCount();
		int removeOps = m_pTestSettings->GetOpRemoveCount();
		ImGui::Text("Insert Ops: %d", insertOps);
		ImGui::Text("Remove Ops: %d", removeOps);
		ImGui::Text("Total Ops: %d", insertOps + removeOps);
		if (ImGui::Button("Reset Ops"))
		{
			m_pTestSettings->Reset();
		}
		ImGui::End();
	}
	void opsPerThread()
	{
		auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - m_pTestSettings->GetLastOpsUpdateTime()).count() >= 1)
		{
			for (int i = 0; i < (int)m_pTestSettings->GetThreadOpCounts().size(); i++)
			{
				int currentCount = m_pTestSettings->GetThreadOpCounts()[i].load();
				m_pTestSettings->SetThreadOpsPerSec(i, currentCount - m_pTestSettings->GetLastThreadCounts(i));
				m_pTestSettings->SetLastThreadCounts(i, currentCount);
			}
			m_pTestSettings->SetLastOpsUpdateTime(now);
		}

		ImGui::Begin("Ops Per Thread (ops/sec)");
		ImVec2 avail = ImGui::GetContentRegionAvail();
		if (avail.y < 100) avail.y = 100;
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImVec2 winPos = ImGui::GetCursorScreenPos();

		int nThreads = m_numThreadsSlider;
		float rowHeight = avail.y / nThreads;
		float maxOpsSec = 1.0f;
		for (int i = 0; i < nThreads; i++)
		{
			maxOpsSec = std::max(maxOpsSec, (float)m_pTestSettings->GetThreadOpsPerSec(i));
		}
		float barMaxWidth = avail.x;
		float padding = 4.0f;
		for (int i = 0; i < nThreads; i++)
		{
			float ops = (float)m_pTestSettings->GetThreadOpsPerSec(i);
			float barWidth = (ops / maxOpsSec) * barMaxWidth;
			float barY = winPos.y + i * rowHeight;
			ImU32 col = IM_COL32(48, 89, 255, 255);
			ImVec2 barStart(winPos.x, barY + padding);
			ImVec2 barEnd(winPos.x + barWidth, barY + rowHeight - padding);
			draw_list->AddRectFilled(barStart, barEnd, col);
			char buf[64];
			sprintf_s(buf, "Thread %d: %.0f ops/sec", i, ops);
			draw_list->AddText(ImVec2(winPos.x + (barWidth * 0.15f), barY + rowHeight * 0.35f), IM_COL32(255, 255, 255, 510), buf);
		}
		ImGui::End();
	}

};

