var grid;

var options = {
	enableCellNavigation: true,
	enableColumnReorder: true,
	editable: false
};

var data = [];
var search_columns = [];
var columns = [];

async function get_columns() {
	try {
		const response = await fetch('http://localhost:5000/get_columns');
		if (!response.ok) {
			throw new Error('Network response was not ok');
		}
		const data = await response.json();
		
		// Calculate column width
		const gridContainerWidth = document.getElementById('alt-view').offsetWidth * 0.8;
		const numColumns = data.columns.length;
		const widthPerColumn = gridContainerWidth / Math.min(5, numColumns);
		const minWidthPerColumn = gridContainerWidth / Math.min(5, numColumns);
		const columnWidth = Math.max(widthPerColumn, minWidthPerColumn);

		const columns = data.columns.map(column => ({
			id: column,
			name: column,
			field: column,
			width: columnWidth,
			formatter: highlightMatchingText
		}));
		return columns;
	} catch (error) {
		console.error('Error fetching columns:', error);
		return [];
	}
}

async function get_search_columns() {
	try {
		const response = await fetch('http://localhost:5000/get_search_columns');
		if (!response.ok) {
			throw new Error('Network response was not ok');
		}
		const data = await response.json();

		return data.columns;

	} catch (error) {
		console.error('Error fetching columns:', error);
		return [];
	}
}

async function waitForPort(port, retryInterval = 1000, maxRetries = 60) {
    let retries = 0;
    while (retries < maxRetries) {
        try {
            // Attempt to fetch a resource from the server on the specified port
            const response = await fetch(`http://localhost:${port}/healthcheck`, { method: 'HEAD' });
            if (response.ok) {
                // Port is available
                console.log(`Server is up on port ${port}`);
                return;
            }
        } catch (error) {
            console.error(`Error fetching healthcheck: ${error.message}`);
            // Wait for the retry interval before checking again
            await new Promise(resolve => setTimeout(resolve, retryInterval));
        }
        retries++;
    }
    console.error(`Server did not start on port ${port} after ${maxRetries} attempts`);
    throw new Error(`Server did not start on port ${port}`);
}

document.addEventListener("DOMContentLoaded", function() {
	(async function() {
		await waitForPort(5000);
		columns = await get_columns();
		grid = new Slick.Grid("#myGrid", data, columns, options);
		search();

		search_columns = await get_search_columns();

		// Create search boxes
		const inputData = [];
		for (let i = 0; i < search_columns.length; i++) {
			const column = search_columns[i];
			inputData.push(
				{ id: `search_box_${column}`, placeholder: `Enter ${column} here...` }
			);
		}

		const container = document.getElementById("search_boxes");

		inputData.forEach(data => {
			const input = document.createElement("input");
			input.type = "text";
			input.id = data.id;
			input.className = "search_box";
			input.placeholder = data.placeholder;
			input.onkeyup = search;

			container.appendChild(input);
		});
	})();
});

function escapeHtml(text) {
    return text
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
}

function highlightMatchingText(row, cell, value, columnDef, dataContext) {
	let search_box = "search_box_" + columnDef.name;
	try {
		var query = document.getElementById(search_box).value;
	} catch (e) {
		var query = false;
	}

	if (!query || typeof value !== 'string' || query === 0) {
		return value;
	}

	// Do for tokens split by space
	var tokens = query.split(" ");
	// Only do first 6 tokens
	tokens = tokens.slice(0, 6);

	var result = escapeHtml(value);
	tokens.forEach(token => {
		// Token must contain at least one alphanumeric character
		if (!/[a-z0-9]/i.test(token)) {
			return;
		}
		var regex = new RegExp('(' + token + ')', 'gi');
		result = result.replace(regex, '<span class="highlight">$1</span>');
	});
	return result;
}


function updateGrid(results) {
	// Update metadata
	var numResultsDiv = document.getElementById('metadata');
	numResultsDiv.innerHTML = `<b>Number of results:</b> ${results.results.length} 
							   &nbsp;&nbsp;&nbsp;&nbsp;
							   <b>Time taken:</b> ${Math.round(results.time_taken_ms)}ms`;

	// Update grid data
	grid.setData(results.results);
	grid.invalidate();
	grid.render();
}

// Define function `search` as GET request to the API
function search() {
	let params = {};
	search_columns.forEach(column => {
		params[column] = document.getElementById(`search_box_${column}`).value;
	});
	params['limit'] = document.getElementById('limit').value;

	let text_string = new URLSearchParams(params).toString();
	let query = `${text_string}`;

	fetch(`http://localhost:5000/search?${query}`)
		.then(response => response.json())
		.then(data => {
			updateGrid(data);
		});
}

grid = new Slick.Grid("#myGrid", data, columns, options);
